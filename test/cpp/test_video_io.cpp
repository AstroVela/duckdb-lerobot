#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "function/lerobot_copy.hpp"
#include "function/lerobot_video_io.hpp"

#include <cstring>
#include <condition_variable>
#include <future>

#ifdef LEROBOT_HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
}
#endif

using namespace duckdb;

namespace {

struct TestDirectory {
	TestDirectory()
	    : fs(FileSystem::CreateLocal()),
	      path(fs->JoinPath(FileSystem::GetWorkingDirectory(),
	                        "build/write_io_" + UUID::ToString(UUID::GenerateRandomUUID()))) {
		fs->CreateDirectoriesRecursive(path);
	}
	~TestDirectory() {
		fs->RemoveDirectory(path);
	}
	unique_ptr<FileSystem> fs;
	string path;
};

struct Event {
	string operation;
	string path;
	bool write;
};

struct FaultPlan {
	bool IsStagingRoot(const string &path) const {
		const auto prefix = publish_root + ".tmp-";
		return !publish_root.empty() && StringUtil::StartsWith(path, prefix) &&
		       path.find('/', prefix.size()) == string::npos;
	}
	bool Matches(const string &path, const string &part) const {
		if (part == "<staging>") {
			return IsStagingRoot(path);
		}
		if (part == "<publish>") {
			return path == publish_root;
		}
		if (part == "<remux>") {
			return remux_started && StringUtil::Contains(path, "episode-") && StringUtil::EndsWith(path, ".mp4");
		}
		return StringUtil::Contains(path, part) &&
		       ((part != "episode-" && part != "shard-") || StringUtil::EndsWith(path, ".mp4"));
	}
	void Arm(string operation_p, string needle_p, bool write_p, bool short_write_p = false) {
		lock_guard<mutex> guard(lock);
		operation = std::move(operation_p);
		needle = std::move(needle_p);
		write = write_p;
		short_write = short_write_p;
		fired = false;
		extra_close_error = false;
		remux_started = false;
		fail_spool_close = false;
		fail_removal = false;
		fail_cleanup_probe = false;
		nonstandard_cleanup_error = false;
		stage_collision = false;
		staging_root.clear();
		events.clear();
		on_match = nullptr;
	}
	bool Visit(const string &op, const string &path, bool is_write) {
		lock_guard<mutex> guard(lock);
		events.push_back({op, path, is_write});
		if (op == "created" && IsStagingRoot(path)) {
			staging_root = path;
		}
		if (fired && fail_spool_close && op == "close" && StringUtil::EndsWith(path, ".raw")) {
			if (nonstandard_cleanup_error) {
				throw 7;
			}
			throw IOException("secondary spool close failure");
		}
		if (IsStagingRoot(path)) {
			if (op == "remove_directory") {
				if (open_handles.load() != 0) {
					throw IOException("staging removal attempted with open handles");
				}
				if (fail_removal) {
					throw IOException("secondary staging removal failure");
				}
			}
			if (op == "exists" && fired && fail_cleanup_probe) {
				throw IOException("secondary staging probe failure");
			}
		}
		if (op == "open" && is_write && StringUtil::Contains(path, "shard-")) {
			remux_started = true;
		}
		if (fired && extra_close_error && op == "close" && Matches(path, needle)) {
			throw IOException("secondary close failure");
		}
		if (!fired && op == operation && is_write == write && Matches(path, needle)) {
			fired = true;
			if (on_match) {
				on_match();
				return false;
			}
			if (short_write) {
				return true;
			}
			throw IOException("injected LeRobot %s failure", op);
		}
		return false;
	}
	idx_t Count(const string &op, const string &path_part, bool is_write) {
		lock_guard<mutex> guard(lock);
		idx_t count = 0;
		for (const auto &event : events) {
			if (event.operation == op && event.write == is_write && Matches(event.path, path_part)) {
				count++;
			}
		}
		return count;
	}

	mutex lock;
	string operation;
	string needle;
	string publish_root;
	bool write = false;
	bool short_write = false;
	bool fired = false;
	bool extra_close_error = false;
	bool remux_started = false;
	bool fail_spool_close = false;
	bool fail_removal = false;
	bool fail_cleanup_probe = false;
	bool nonstandard_cleanup_error = false;
	bool stage_collision = false;
	string staging_root;
	vector<Event> events;
	atomic<idx_t> open_handles {0};
	std::function<void()> on_match;
};

struct TrackedHandle final : public FileHandle {
	TrackedHandle(FileSystem &fs, string path, FileOpenFlags flags, unique_ptr<FileHandle> inner_p, FaultPlan &plan_p)
	    : FileHandle(fs, std::move(path), flags), inner(std::move(inner_p)), plan(plan_p) {
		plan.open_handles++;
	}
	~TrackedHandle() override {
		try {
			Close();
		} catch (...) {
		}
	}
	void Close() override {
		if (inner) {
			inner->Close();
			inner.reset();
			plan.open_handles--;
			// Model close errors after releasing the OS handle, as real close(2)
			// may do. Only an explicit checked Close can observe this failure.
			plan.Visit("close", path, flags.OpenForWriting());
		}
	}
	unique_ptr<FileHandle> inner;
	FaultPlan &plan;
};

class FaultFileSystem final : public FileSystem {
public:
	explicit FaultFileSystem(string root_p) : root(std::move(root_p)), local(FileSystem::CreateLocal()) {
	}
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override {
		plan.Visit("open", path, flags.OpenForWriting());
		return make_uniq<TrackedHandle>(*this, path, flags, local->OpenFile(path, flags, opener), plan);
	}
	void Read(FileHandle &file, void *buffer, int64_t bytes, idx_t location) override {
		auto &handle = file.Cast<TrackedHandle>();
		plan.Visit("read", file.path, false);
		handle.inner->Read(buffer, bytes, location);
	}
	int64_t Read(FileHandle &file, void *buffer, int64_t bytes) override {
		plan.Visit("read", file.path, false);
		return file.Cast<TrackedHandle>().inner->Read(buffer, bytes);
	}
	void Write(FileHandle &file, void *buffer, int64_t bytes, idx_t location) override {
		if (plan.Visit("write", file.path, true)) {
			throw IOException("injected positional short write");
		}
		auto &inner = *file.Cast<TrackedHandle>().inner;
		inner.file_system.Write(inner, buffer, bytes, location);
	}
	int64_t Write(FileHandle &file, void *buffer, int64_t bytes) override {
		const bool short_write = plan.Visit("write", file.path, true);
		return file.Cast<TrackedHandle>().inner->Write(buffer, short_write ? bytes / 2 : bytes);
	}
	int64_t GetFileSize(FileHandle &file) override {
		if (invalid_size) {
			return -1;
		}
		return file.Cast<TrackedHandle>().inner->GetFileSize();
	}
	FileType GetFileType(FileHandle &file) override {
		return file.Cast<TrackedHandle>().inner->GetType();
	}
	timestamp_t GetLastModifiedTime(FileHandle &file) override {
		return local->GetLastModifiedTime(*file.Cast<TrackedHandle>().inner);
	}
	void Seek(FileHandle &file, idx_t position) override {
		plan.Visit("seek", file.path, file.flags.OpenForWriting());
		file.Cast<TrackedHandle>().inner->Seek(position);
	}
	idx_t SeekPosition(FileHandle &file) override {
		return file.Cast<TrackedHandle>().inner->SeekPosition();
	}
	void FileSync(FileHandle &file) override {
		plan.Visit("sync", file.path, true);
		file.Cast<TrackedHandle>().inner->Sync();
	}
	void Truncate(FileHandle &file, int64_t size) override {
		file.Cast<TrackedHandle>().inner->Truncate(size);
	}
	bool OnDiskFile(FileHandle &) override {
		return true;
	}
	bool CanSeek() override {
		return true;
	}
	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, root + "/");
	}
	string GetName() const override {
		return "LeRobot video fault filesystem";
	}
	bool FileExists(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		if (plan.stage_collision && plan.IsStagingRoot(path)) {
			// Model a pre-existing directory at the generated staging name.
			local->CreateDirectoriesRecursive(path);
			auto marker =
			    local->OpenFile(path + "/keep", FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
			marker->Close();
			plan.staging_root = path;
			return false;
		}
		return local->FileExists(path, opener);
	}
	bool DirectoryExists(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		plan.Visit("exists", path, false);
		return local->DirectoryExists(path, opener);
	}
	bool IsPipe(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		return local->IsPipe(path, opener);
	}
	void CreateDirectory(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		local->CreateDirectory(path, opener);
		plan.Visit("created", path, true);
	}
	void RemoveDirectory(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		plan.Visit("remove_directory", path, true);
		local->RemoveDirectory(path, opener);
	}
	bool ListFiles(const string &path, const std::function<void(const string &, bool)> &callback,
	               FileOpener *opener = nullptr) override {
		return local->ListFiles(path, callback, opener);
	}
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener = nullptr) override {
		plan.Visit("move", target, true);
		local->MoveFile(source, target, opener);
	}
	void RemoveFile(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		local->RemoveFile(path, opener);
	}
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override {
		return local->Glob(path, opener);
	}

	FaultPlan plan;
	string root;
	unique_ptr<FileSystem> local;
	bool invalid_size = false;
};

#ifdef LEROBOT_HAVE_FFMPEG
struct IOFile {
	IOFile(FileSystem &fs, const string &path, bool write, optional_ptr<atomic<bool>> cancelled = nullptr)
	    : io(fs, path, write, cancelled), format(avformat_alloc_context()) {
		if (!format) {
			throw OutOfMemoryException("test format");
		}
		try {
			io.Open(*format);
		} catch (...) {
			avformat_free_context(format);
			throw;
		}
	}
	~IOFile() {
		io.Close(format->pb);
		avformat_free_context(format);
	}
	LerobotVideoIO io;
	AVFormatContext *format;
};
#endif

struct CopyTest {
	CopyTest() {
		DBConfig config;
		config.options.load_extensions = false;
		config.options.maximum_threads = 2;
		db = make_uniq<DuckDB>(nullptr, &config);
		for (const auto &extension : {"core_functions", "json", "parquet"}) {
			REQUIRE(ExtensionHelper::LoadExtension(*db, extension) == ExtensionLoadResult::LOADED_EXTENSION);
		}
		connection = make_uniq<Connection>(*db);
		auto system = make_uniq<FaultFileSystem>(directory.path);
		fs = system.get();
		FileSystem::GetFileSystem(*db->instance).RegisterSubSystem(std::move(system));
		connection->BeginTransaction();
		CreateCopyFunctionInfo info(LerobotCopyFunction::Create());
		Catalog::GetSystemCatalog(*connection->context).CreateCopyFunction(*connection->context, info);
		connection->Commit();
		Query("SET enable_external_file_cache = false");
		root = directory.path + "/dataset ' # 机器人";
		fs->plan.publish_root = root;
	}
	void Query(const string &sql) {
		auto result = connection->Query(sql);
		INFO((result->HasError() ? result->GetError() : ""));
		REQUIRE_FALSE(result->HasError());
	}
	unique_ptr<MaterializedQueryResult> Copy() {
		return connection->Query(
		    "COPY (SELECT episode::BIGINT AS episode_index, 'io test' AS task, "
		    "from_hex(repeat('112233', 6144)) AS camera, from_hex(repeat('805020', 8192)) AS wrist "
		    "FROM range(2) episodes(episode), range(2) frames(frame) ORDER BY episode, frame) TO " +
		    Value(root).ToSQLString() +
		    " (FORMAT lerobot, FPS 30, VIDEO_WORKERS 2, ENCODER_THREADS 2, RGB_CODEC 'libaom-av1', "
		    "FEATURES '{\"camera\":{\"dtype\":\"video\",\"shape\":[64,96,3]},"
		    "\"wrist\":{\"dtype\":\"video\",\"shape\":[64,128,3]}}')");
	}
	unique_ptr<MaterializedQueryResult> CopyNumeric() {
		return connection->Query(
		    "COPY (SELECT 0::BIGINT AS episode_index, 'cleanup test' AS task, 7::FLOAT AS action) TO " +
		    Value(root).ToSQLString() +
		    " (FORMAT lerobot, FPS 30, FEATURES '{\"action\":{\"dtype\":\"float32\",\"shape\":[1]}}')");
	}
	string ImageSQL(bool invalid = false) {
		const auto small = "from_hex(repeat(printf('%06x', (i * 7171)::INTEGER), 15))";
		return "COPY (SELECT (i // 3)::BIGINT AS episode_index, 'PNG test' AS task, " +
		       (invalid ? "CASE WHEN i=4 THEN 'x'::BLOB ELSE " + string(small) + " END" : string(small)) +
		       " AS camera, from_hex(repeat(printf('%06x', (i * 1717)::INTEGER), 527)) AS wrist "
		       "FROM range(6) t(i) ORDER BY i) TO " +
		       Value(root).ToSQLString() +
		       " (FORMAT lerobot, FPS 30, FEATURES '{\"camera\":{\"dtype\":\"image\",\"shape\":[3,5,3]},"
		       "\"wrist\":{\"dtype\":\"image\",\"shape\":[17,31,3]}}')";
	}
	vector<string> CleanupMessages() {
		auto result =
		    connection->Query("SELECT message FROM duckdb_logs WHERE starts_with(message, 'LeRobot cleanup failed')");
		REQUIRE_FALSE(result->HasError());
		vector<string> messages;
		for (idx_t row = 0; row < result->RowCount(); row++) {
			messages.push_back(result->GetValue(0, row).ToString());
		}
		return messages;
	}
	void CheckRollback() {
		REQUIRE(fs->plan.open_handles.load() == 0);
		REQUIRE_FALSE(directory.fs->DirectoryExists(root));
		vector<string> entries;
		directory.fs->ListFiles(directory.path, [&](const string &name, bool) { entries.push_back(name); });
		REQUIRE(entries.empty());
		auto result = connection->Query("SELECT 42");
		REQUIRE_FALSE(result->HasError());
		REQUIRE(result->GetValue(0, 0).GetValue<int>() == 42);
	}

	TestDirectory directory;
	unique_ptr<DuckDB> db;
	unique_ptr<Connection> connection;
	FaultFileSystem *fs;
	string root;
};

} // namespace

TEST_CASE("COPY timestamps round only after division, including FPS beyond float integer precision",
          "[numeric_stats]") {
	CopyTest test;
	test.Query("COPY (SELECT 0::BIGINT AS episode_index, 'timestamp' AS task, i::FLOAT AS action "
	           "FROM range(2) t(i) ORDER BY i) TO " +
	           Value(test.root).ToSQLString() +
	           " (FORMAT lerobot, FPS 16777217, FEATURES '{\"action\":{\"dtype\":\"float32\",\"shape\":[1]}}')");
	auto result =
	    test.connection->Query("SELECT timestamp FROM read_parquet(" +
	                           Value(test.root + "/data/**/*.parquet").ToSQLString() + ") ORDER BY frame_index");
	REQUIRE_FALSE(result->HasError());
	REQUIRE(result->RowCount() == 2);
	REQUIRE(result->GetValue(0, 0).GetValue<float>() == 0);
	const auto timestamp = result->GetValue(0, 1).GetValue<float>();
	uint32_t bits;
	memcpy(&bits, &timestamp, sizeof(bits));
	// Independently recorded from LeRobot 0.6.1 / Arrow float32 output.
	REQUIRE(bits == 864026623);
}

TEST_CASE("COPY numeric statistics keep scanned buffers valid across dimension passes", "[numeric_stats]") {
	CopyTest test;
	test.Query("COPY (SELECT 0::BIGINT AS episode_index, 'stats' AS task, "
	           "list_transform(range(129), d -> (i + d)::FLOAT / 8)::FLOAT[129] AS action, "
	           "(9007199254740993::BIGINT + i)::BIGINT AS counter "
	           "FROM range(4097) t(i) ORDER BY i) TO " +
	           Value(test.root).ToSQLString() +
	           " (FORMAT lerobot, FPS 30, FEATURES '{\"action\":{\"dtype\":\"float32\",\"shape\":[129]},"
	           "\"counter\":{\"dtype\":\"int64\",\"shape\":[1]}}')");
	auto result =
	    test.connection->Query("SELECT \"stats/action/mean\"[129], \"stats/action/count\"[1], "
	                           "\"stats/counter/min\"[1], \"stats/counter/max\"[1] FROM read_parquet(" +
	                           Value(test.root + "/meta/episodes/chunk-000/file-000.parquet").ToSQLString() + ")");
	REQUIRE_FALSE(result->HasError());
	REQUIRE(result->RowCount() == 1);
	REQUIRE(result->GetValue(0, 0).GetValue<double>() == 272.0);
	REQUIRE(result->GetValue(1, 0).GetValue<int64_t>() == 4097);
	REQUIRE(result->GetValue(2, 0).GetValue<int64_t>() == 9007199254740993LL);
	REQUIRE(result->GetValue(3, 0).GetValue<int64_t>() == 9007199254745089LL);
}

#ifdef LEROBOT_HAVE_FFMPEG
TEST_CASE("Image COPY rolls back populated PNG writers on input and filesystem errors", "[image_copy]") {
	struct Fault {
		const char *operation;
		const char *needle;
	};
	for (const auto &fault : {Fault {"write", "/0/episode-1.raw"}, Fault {"write", ".parquet"},
	                          Fault {"move", "<publish>"}, Fault {"", ""}}) {
		DYNAMIC_SECTION(fault.operation << " " << fault.needle) {
			CopyTest test;
			const bool invalid = string(fault.operation).empty();
			test.fs->plan.Arm(fault.operation, fault.needle, true);
			auto failed = test.connection->Query(test.ImageSQL(invalid));
			REQUIRE(failed->HasError());
			REQUIRE(failed->GetErrorType() == (invalid ? ExceptionType::INVALID_INPUT : ExceptionType::IO));
			REQUIRE(test.fs->plan.fired == !invalid);
			test.CheckRollback();
			test.fs->plan.Arm("", "", false);
			test.Query(test.ImageSQL());
			REQUIRE(test.fs->plan.open_handles.load() == 0);
			auto rows = test.connection->Query("SELECT count(*) FROM read_parquet(" +
			                                   Value(test.root + "/data/**/*.parquet").ToSQLString() + ")");
			REQUIRE_FALSE(rows->HasError());
			REQUIRE(rows->GetValue(0, 0).GetValue<int64_t>() == 6);
		}
	}
}

namespace {

// Pause a real COPY after both PNG writers have encoded the first episode.
// The filesystem only gates progress; the caller issues the actual interrupt.
struct ImageCopyRun {
	explicit ImageCopyRun(CopyTest &test_p) : test(test_p) {
	}
	~ImageCopyRun() {
		if (future.valid()) {
			test.connection->Interrupt();
			Release();
			future.wait();
		}
	}
	void Pause() {
		unique_lock<mutex> guard(lock);
		entered = true;
		cv.notify_all();
		cv.wait(guard, [&]() { return released; });
	}
	bool WaitUntilEntered() {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&]() { return entered; });
	}
	void Release() {
		lock_guard<mutex> guard(lock);
		released = true;
		cv.notify_all();
	}
	CopyTest &test;
	mutex lock;
	std::condition_variable cv;
	bool entered = false;
	bool released = false;
	std::future<unique_ptr<MaterializedQueryResult>> future;
};

} // namespace

TEST_CASE("Interrupting image COPY releases PNG writers and permits retry on the same database", "[image_copy]") {
	for (const auto threads : {1, 4}) {
		DYNAMIC_SECTION("threads=" << threads) {
			CopyTest test;
			test.Query("SET threads=" + std::to_string(threads));
			for (idx_t repeat = 0; repeat < 3; repeat++) {
				ImageCopyRun running(test);
				test.fs->plan.Arm("write", "/1/episode-1.raw", true);
				test.fs->plan.on_match = [&]() {
					running.Pause();
				};
				running.future =
				    std::async(std::launch::async, [&]() { return test.connection->Query(test.ImageSQL()); });
				REQUIRE(running.WaitUntilEntered());
				Connection other(*test.db);
				REQUIRE_FALSE(other.Query("SELECT 42")->HasError());
				test.connection->Interrupt();
				running.Release();
				REQUIRE(running.future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
				auto failed = running.future.get();
				REQUIRE(failed->HasError());
				REQUIRE(failed->GetErrorType() == ExceptionType::INTERRUPT);
				test.CheckRollback();
			}
			test.fs->plan.Arm("", "", false);
			test.Query(test.ImageSQL());
			REQUIRE(test.fs->plan.open_handles.load() == 0);
		}
	}
}

TEST_CASE("Video AVIO checks buffered write, sync and close failures", "[video_io]") {
	for (const auto &operation : {"write", "sync", "close"}) {
		for (bool secondary : {false, true}) {
			CAPTURE(operation, secondary);
			TestDirectory directory;
			FaultFileSystem fs(directory.path);
			IOFile file(fs, directory.path + "/output.mp4", true);
			fs.plan.Arm(operation, "output.mp4", true);
			fs.plan.extra_close_error = secondary;
			const uint8_t bytes[] = {1, 2, 3, 4};
			avio_write(file.format->pb, bytes, sizeof(bytes));
			// Small writes are still buffered. Close must surface the failure,
			// release its handle, and preserve it ahead of any cleanup error.
			REQUIRE_FALSE(fs.plan.fired);
			file.io.Close(file.format->pb);
			REQUIRE(fs.plan.fired);
			REQUIRE(fs.plan.open_handles.load() == 0);
			REQUIRE_THROWS_WITH(file.io.ThrowIfError(), Catch::Contains("injected LeRobot " + string(operation)));
		}
	}
}

TEST_CASE("Interrupting final video assembly prevents publication and permits retry", "[video_copy_cancel]") {
	struct Phase {
		const char *operation;
		const char *path;
		bool write;
	};
	for (const auto &phase : {Phase {"open", "shard-", true}, Phase {"read", "<remux>", false},
	                          Phase {"open", "shard-", false}, Phase {"write", "/meta/info.json", true}}) {
		DYNAMIC_SECTION(phase.operation << " " << phase.path << " write=" << phase.write) {
			CopyTest test;
			test.fs->plan.Arm(phase.operation, phase.path, phase.write);
			test.fs->plan.on_match = [&]() {
				test.connection->Interrupt();
			};
			auto result = test.Copy();
			REQUIRE(test.fs->plan.fired);
			REQUIRE(result->HasError());
			REQUIRE(result->GetErrorType() == ExceptionType::INTERRUPT);
			test.CheckRollback();
			test.fs->plan.Arm("", "", false);
			REQUIRE_FALSE(test.Copy()->HasError());
			REQUIRE(test.fs->plan.open_handles.load() == 0);
		}
	}
}

TEST_CASE("Video AVIO rejects short writes and propagates cancellation", "[video_io]") {
	for (bool cancelled : {false, true}) {
		CAPTURE(cancelled);
		TestDirectory directory;
		FaultFileSystem fs(directory.path);
		atomic<bool> stop {false};
		IOFile file(fs, directory.path + "/output.mp4", true, &stop);
		fs.plan.Arm("write", "output.mp4", true, true);
		const uint8_t bytes[] = {1, 2, 3, 4};
		avio_write(file.format->pb, bytes, sizeof(bytes));
		stop = cancelled;
		file.io.Close(file.format->pb);
		REQUIRE(fs.plan.open_handles.load() == 0);
		if (cancelled) {
			REQUIRE_FALSE(fs.plan.fired);
			REQUIRE_THROWS_AS(file.io.ThrowIfError(), InterruptException);
		} else {
			REQUIRE(fs.plan.fired);
			REQUIRE_THROWS_WITH(file.io.ThrowIfError(), Catch::Contains("wrote 2 of 4 bytes"));
		}
	}
}

TEST_CASE("Video AVIO rejects secondary paths and releases abandoned streams", "[video_io]") {
	TestDirectory directory;
	FaultFileSystem fs(directory.path);
	{
		IOFile file(fs, directory.path + "/output.mp4", true);
		AVIOContext *unexpected = nullptr;
		REQUIRE(file.format->io_open(file.format, &unexpected, "file:/outside.mp4", AVIO_FLAG_READ, nullptr) < 0);
		REQUIRE(unexpected == nullptr);
		REQUIRE_THROWS_WITH(file.io.ThrowIfError(), Catch::Contains("Unexpected FFmpeg I/O request"));
	}
	REQUIRE(fs.plan.open_handles.load() == 0);
	// Model avformat_open_input freeing its format after CUSTOM_IO was attached.
	{
		LerobotVideoIO io(fs, directory.path + "/abandoned.mp4", true);
		auto format = avformat_alloc_context();
		REQUIRE(format != nullptr);
		io.Open(*format);
		avformat_free_context(format);
	}
	REQUIRE(fs.plan.open_handles.load() == 0);
}

TEST_CASE("Video AVIO reads and seeks without hiding filesystem errors", "[video_io]") {
	TestDirectory directory;
	FaultFileSystem fs(directory.path);
	const auto path = directory.path + "/input.mp4";
	vector<uint8_t> bytes(200000);
	for (idx_t index = 0; index < bytes.size(); index++) {
		bytes[index] = index % 251;
	}
	{
		IOFile output(fs, path, true);
		avio_write(output.format->pb, bytes.data(), bytes.size());
		output.io.Close(output.format->pb);
		REQUIRE_NOTHROW(output.io.ThrowIfError());
	}
	SECTION("size, EOF, buffered and filesystem seeks") {
		IOFile input(fs, path, false);
		auto pb = input.format->pb;
		REQUIRE(avio_size(pb) == static_cast<int64_t>(bytes.size()));
		uint8_t buffer[17];
		for (const int64_t position : {0, 5, 123456, 10, 199983}) {
			REQUIRE(avio_seek(pb, position, SEEK_SET) == position);
			REQUIRE(avio_read(pb, buffer, sizeof(buffer)) == sizeof(buffer));
			REQUIRE(memcmp(buffer, bytes.data() + position, sizeof(buffer)) == 0);
		}
		REQUIRE(avio_read(pb, buffer, sizeof(buffer)) == AVERROR_EOF);
		REQUIRE(pb->seek(pb->opaque, -1, SEEK_SET) < 0);
		REQUIRE(pb->seek(pb->opaque, NumericLimits<int64_t>::Maximum(), SEEK_END) < 0);
		REQUIRE(pb->read_packet(pb->opaque, buffer, -1) < 0);
		REQUIRE_NOTHROW(input.io.ThrowIfError());
	}
	for (const auto &operation : {"read", "seek"}) {
		DYNAMIC_SECTION("original " << operation << " error") {
			IOFile input(fs, path, false);
			fs.plan.Arm(operation, "input.mp4", false);
			auto pb = input.format->pb;
			if (string(operation) == "read") {
				uint8_t buffer[17];
				REQUIRE(avio_read(pb, buffer, sizeof(buffer)) < 0);
			} else {
				REQUIRE(pb->seek(pb->opaque, 100000, SEEK_SET) < 0);
			}
			input.io.Close(input.format->pb);
			REQUIRE(fs.plan.open_handles.load() == 0);
			REQUIRE_THROWS_WITH(input.io.ThrowIfError(), Catch::Contains("injected LeRobot " + string(operation)));
		}
	}
	SECTION("invalid filesystem size is an I/O error") {
		IOFile input(fs, path, false);
		fs.invalid_size = true;
		REQUIRE(avio_size(input.format->pb) < 0);
		REQUIRE_THROWS_AS(input.io.ThrowIfError(), IOException);
	}
}

TEST_CASE("COPY video filesystem failures prevent publication and allow retry", "[copy_io]") {
	struct Fault {
		const char *operation;
		const char *needle;
		bool write;
		bool short_write;
	};
	const Fault faults[] = {
	    {"open", "episode-", true, false},   {"write", "episode-", true, false}, {"write", "episode-", true, true},
	    {"seek", "episode-", true, false},   {"sync", "episode-", true, false},  {"close", "episode-", true, false},
	    {"open", "episode-", false, false},  {"read", "episode-", false, false}, {"close", "episode-", false, false},
	    {"write", "shard-", true, false},    {"write", "shard-", true, true},    {"seek", "shard-", true, false},
	    {"sync", "shard-", true, false},     {"close", "shard-", true, false},   {"read", "shard-", false, false},
	    {"close", "shard-", false, false},   {"read", "<remux>", false, false},  {"write", "info.json", true, true},
	    {"close", "info.json", true, false}, {"write", ".parquet", true, false}, {"move", "/videos/", true, false},
	    {"move", "<publish>", true, false},  {"close", ".parquet", true, false}};
	for (const auto &fault : faults) {
		DYNAMIC_SECTION(fault.operation << " " << fault.needle << " write=" << fault.write
		                                << " short=" << fault.short_write) {
			CopyTest test;
			test.fs->plan.Arm(fault.operation, fault.needle, fault.write, fault.short_write);
			auto failed = test.Copy();
			INFO((failed->HasError() ? failed->GetError() : "COPY unexpectedly succeeded"));
			string moves;
			for (const auto &event : test.fs->plan.events) {
				if (event.operation == "move") {
					moves += event.path + "\n";
				}
			}
			INFO(moves);
			REQUIRE(test.fs->plan.fired);
			REQUIRE(failed->HasError());
			if (fault.short_write) {
				REQUIRE(StringUtil::Contains(failed->GetError(), "Failed to write complete LeRobot"));
			} else {
				REQUIRE(StringUtil::Contains(failed->GetError(), "injected LeRobot " + string(fault.operation)));
			}
			test.CheckRollback();
			// A successful retry uses the same root, database and shared worker pool.
			test.fs->plan.Arm("", "", false);
			auto retry = test.Copy();
			INFO((retry->HasError() ? retry->GetError() : ""));
			string entries;
			test.directory.fs->ListFiles(test.directory.path,
			                             [&](const string &name, bool) { entries += name + "\n"; });
			INFO(entries);
			REQUIRE_FALSE(retry->HasError());
			REQUIRE(test.fs->plan.open_handles.load() == 0);
			REQUIRE(test.directory.fs->FileExists(test.root + "/meta/info.json"));
			REQUIRE(test.fs->plan.Count("open", "episode-", true) == 4);
			REQUIRE(test.fs->plan.Count("open", "shard-", true) == 2);
			// Episode faststart, fragment remux and shard faststart all use DuckDB.
			REQUIRE(test.fs->plan.Count("open", "episode-", false) >= 8);
			REQUIRE(test.fs->plan.Count("open", "shard-", false) == 2);
		}
	}
}

TEST_CASE("COPY continues cleanup after spool and diagnostic failures", "[copy_cleanup_media]") {
	for (bool removal_error : {false, true}) {
		for (bool warnings_as_errors : {false, true}) {
			for (bool nonstandard : {false, true}) {
				DYNAMIC_SECTION("remove=" << removal_error << " warnings_as_errors=" << warnings_as_errors
				                          << " nonstandard=" << nonstandard) {
					CopyTest test;
					test.Query("SET enable_logging=true; SET logging_level='WARNING'");
					if (warnings_as_errors) {
						test.Query("SET warnings_as_errors=true");
					}
					// Fail during the second episode, with both visual spools and
					// the previous episode's native Parquet writer still open.
					auto &plan = test.fs->plan;
					plan.Arm("write", "/1/episode-1.raw", true);
					plan.fail_spool_close = true;
					plan.fail_removal = removal_error;
					plan.nonstandard_cleanup_error = nonstandard;
					auto failed = test.Copy();
					REQUIRE(plan.fired);
					REQUIRE(failed->HasError());
					REQUIRE(failed->GetErrorType() == ExceptionType::IO);
					REQUIRE(failed->GetError() == "IO Error: injected LeRobot write failure");
					REQUIRE(plan.Count("open", ".parquet", true) >= 1);
					REQUIRE(plan.Count("close", "episode-1.raw", true) == 2);
					REQUIRE(plan.open_handles.load() == 0);
					REQUIRE(plan.Count("remove_directory", "<staging>", true) == 1);
					REQUIRE_FALSE(test.directory.fs->DirectoryExists(test.root));
					REQUIRE(test.directory.fs->DirectoryExists(plan.staging_root) == removal_error);
					auto messages = test.CleanupMessages();
					if (warnings_as_errors) {
						// DuckDB throws before writing the warning; cleanup must
						// still finish and the primary error must remain intact.
						REQUIRE(messages.empty());
					} else {
						REQUIRE(messages.size() == (removal_error ? 3 : 2));
						idx_t spool_errors = 0;
						for (const auto &message : messages) {
							REQUIRE(StringUtil::Contains(message, plan.staging_root));
							if (StringUtil::Contains(message, "close visual spool")) {
								spool_errors++;
								REQUIRE(StringUtil::Contains(message, "episode-1.raw"));
								REQUIRE(StringUtil::Contains(message, nonstandard ? "unknown exception"
								                                                  : "secondary spool close failure"));
							} else {
								REQUIRE(StringUtil::Contains(message, "secondary staging removal failure"));
							}
						}
						REQUIRE(spool_errors == 2);
					}
					if (removal_error) {
						// Simulate removal becoming available again, using the
						// exact residual directory reported in the diagnostic.
						test.directory.fs->RemoveDirectory(plan.staging_root);
					}
					test.CheckRollback();
					plan.Arm("", "", false);
					auto retry = test.Copy();
					REQUIRE_FALSE(retry->HasError());
					REQUIRE(plan.open_handles.load() == 0);
					REQUIRE(plan.Count("remove_directory", "<staging>", true) == 0);
					REQUIRE(test.directory.fs->FileExists(test.root + "/meta/info.json"));
				}
			}
		}
	}
}
#endif

TEST_CASE("Interrupting numeric COPY before publication rolls back the dataset", "[copy_cleanup]") {
	CopyTest test;
	test.fs->plan.Arm("write", "/meta/info.json", true);
	test.fs->plan.on_match = [&]() {
		test.connection->Interrupt();
	};
	auto result = test.CopyNumeric();
	REQUIRE(test.fs->plan.fired);
	REQUIRE(result->HasError());
	REQUIRE(result->GetErrorType() == ExceptionType::INTERRUPT);
	test.CheckRollback();
	test.fs->plan.Arm("", "", false);
	REQUIRE_FALSE(test.CopyNumeric()->HasError());
	REQUIRE(test.fs->plan.open_handles.load() == 0);
}

TEST_CASE("COPY staging is owned during partial construction", "[copy_cleanup]") {
	for (bool removal_error : {false, true}) {
		DYNAMIC_SECTION("remove=" << removal_error) {
			CopyTest test;
			test.Query("SET enable_logging=true; SET logging_level='WARNING'");
			auto &plan = test.fs->plan;
			// The filesystem creates the directory and then reports an error,
			// before the complete COPY state can have a destructor.
			plan.Arm("created", "<staging>", true);
			plan.fail_removal = removal_error;
			auto failed = test.CopyNumeric();
			REQUIRE(plan.fired);
			REQUIRE(failed->HasError());
			REQUIRE(failed->GetErrorType() == ExceptionType::IO);
			REQUIRE(failed->GetError() == "IO Error: injected LeRobot created failure");
			REQUIRE(plan.Count("remove_directory", "<staging>", true) == 1);
			REQUIRE(plan.open_handles.load() == 0);
			REQUIRE_FALSE(test.directory.fs->DirectoryExists(test.root));
			REQUIRE(test.directory.fs->DirectoryExists(plan.staging_root) == removal_error);
			auto messages = test.CleanupMessages();
			REQUIRE(messages.size() == (removal_error ? 1 : 0));
			if (removal_error) {
				REQUIRE(StringUtil::Contains(messages[0], plan.staging_root));
				REQUIRE(StringUtil::Contains(messages[0], "secondary staging removal failure"));
				test.directory.fs->RemoveDirectory(plan.staging_root);
			}
			test.CheckRollback();
			plan.Arm("", "", false);
			REQUIRE_FALSE(test.CopyNumeric()->HasError());
			REQUIRE(plan.Count("remove_directory", "<staging>", true) == 0);
			REQUIRE(test.directory.fs->FileExists(test.root + "/meta/info.json"));
		}
	}
}

TEST_CASE("COPY reports the residual staging path without replacing its error", "[copy_cleanup]") {
	for (bool probe_error : {false, true}) {
		DYNAMIC_SECTION("probe=" << probe_error) {
			CopyTest test;
			test.Query("SET enable_logging=true; SET logging_level='WARNING'");
			auto &plan = test.fs->plan;
			plan.Arm("write", "info.json", true);
			plan.fail_removal = !probe_error;
			plan.fail_cleanup_probe = probe_error;
			auto failed = test.CopyNumeric();
			REQUIRE(plan.fired);
			REQUIRE(failed->HasError());
			REQUIRE(failed->GetErrorType() == ExceptionType::IO);
			REQUIRE(failed->GetError() == "IO Error: injected LeRobot write failure");
			REQUIRE(plan.open_handles.load() == 0);
			REQUIRE_FALSE(test.directory.fs->DirectoryExists(test.root));
			REQUIRE(test.directory.fs->DirectoryExists(plan.staging_root));
			REQUIRE(plan.Count("remove_directory", "<staging>", true) == (probe_error ? 0 : 1));
			auto messages = test.CleanupMessages();
			REQUIRE(messages.size() == 1);
			REQUIRE(StringUtil::Contains(messages[0], plan.staging_root));
			REQUIRE(StringUtil::Contains(messages[0], probe_error ? "secondary staging probe failure"
			                                                      : "secondary staging removal failure"));
			const auto residual_stage = plan.staging_root;
			test.Query("SELECT 42");
			plan.Arm("", "", false);
			// A fresh COPY owns a fresh UUID directory. It can publish without
			// deleting or claiming the previous failed COPY's residual files.
			plan.fail_removal = true;
			REQUIRE_FALSE(test.CopyNumeric()->HasError());
			REQUIRE(plan.open_handles.load() == 0);
			REQUIRE(plan.Count("remove_directory", "<staging>", true) == 0);
			REQUIRE(test.directory.fs->FileExists(test.root + "/meta/info.json"));
			REQUIRE(test.directory.fs->DirectoryExists(residual_stage));
			REQUIRE(test.CleanupMessages() == messages);
			test.directory.fs->RemoveDirectory(residual_stage);
		}
	}
}

TEST_CASE("COPY cleanup preserves directories it does not own", "[copy_cleanup]") {
	CopyTest test;
	test.Query("SET enable_logging=true; SET logging_level='WARNING'");
	auto &plan = test.fs->plan;
	plan.stage_collision = true;
	auto failed = test.CopyNumeric();
	REQUIRE(failed->HasError());
	REQUIRE(StringUtil::Contains(failed->GetError(), "LeRobot staging root already exists"));
	REQUIRE_FALSE(plan.staging_root.empty());
	const auto foreign_stage = plan.staging_root;
	REQUIRE(test.directory.fs->FileExists(foreign_stage + "/keep"));
	REQUIRE(plan.Count("remove_directory", "<staging>", true) == 0);
	REQUIRE(test.CleanupMessages().empty());
	plan.stage_collision = false;
	REQUIRE_FALSE(test.CopyNumeric()->HasError());
	REQUIRE(test.directory.fs->FileExists(test.root + "/meta/info.json"));
	REQUIRE(test.directory.fs->FileExists(foreign_stage + "/keep"));
	// Refusing an already-published root also leaves both directories intact.
	failed = test.CopyNumeric();
	REQUIRE(failed->HasError());
	REQUIRE(StringUtil::Contains(failed->GetError(), "LeRobot dataset root already exists"));
	REQUIRE(plan.Count("remove_directory", "<staging>", true) == 0);
	REQUIRE(test.directory.fs->FileExists(test.root + "/meta/info.json"));
	REQUIRE(test.directory.fs->FileExists(foreign_stage + "/keep"));
	REQUIRE(test.CleanupMessages().empty());
}
