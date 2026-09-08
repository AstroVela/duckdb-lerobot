#define CATCH_CONFIG_MAIN
#include "catch.hpp"

// Exercise the actual timestamp lookup without exposing private reader APIs.
#include "../../src/function/video/lerobot_video_frames.cpp"

#include "duckdb.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

#include <future>
#include <functional>

using namespace duckdb;

namespace {

struct TestDirectory {
	TestDirectory()
	    : fs(FileSystem::CreateLocal()), path("build/lerobot_nested_" + UUID::ToString(UUID::GenerateRandomUUID())) {
		fs->CreateDirectoriesRecursive(path + "/meta/episodes/chunk-000");
	}
	~TestDirectory() {
		fs->RemoveDirectory(path);
	}
	unique_ptr<FileSystem> fs;
	string path;
};

// Only Read is gated: fingerprint Open/stat calls still run normally. The gate
// observes the context supplied by DuckDB's native JSON/Parquet reader, never
// the outer context whose cancellation it is intended to test.
struct ReadGate {
	void Arm(const string &path_p) {
		lock_guard<mutex> guard(lock);
		path = path_p;
		entered = cancelled = released = false;
	}

	void Read(const string &read_path, optional_ptr<ClientContext> reader) {
		unique_lock<mutex> guard(lock);
		if (read_path != path || released) {
			return;
		}
		if (should_pause && !should_pause()) {
			return;
		}
		if (!reader) {
			throw IOException("Gated native reader has no ClientContext");
		}
		if (first_reader_only && entered && reader_context.lock().get() != reader.get()) {
			return;
		}
		reader_context = reader->shared_from_this();
		entered = true;
		cv.notify_all();
		while (!released) {
			if (reader->IsInterrupted()) {
				if (clear_first_interrupt) {
					// Simulate query startup resetting an early interrupt signal.
					reader->ClearInterrupt();
					clear_first_interrupt = false;
					continue;
				}
				cancelled = true;
				cv.notify_all();
			}
			cv.wait_for(guard, std::chrono::milliseconds(1));
		}
		if (cancelled) {
			// File systems may wrap cancellation as an I/O error. The outer
			// query must still receive INTERRUPT, not a misleading bind error.
			throw IOException("Controlled read stopped after interruption");
		}
	}

	bool Wait(bool ReadGate::*flag) {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&] { return this->*flag; });
	}

	void Release() {
		lock_guard<mutex> guard(lock);
		released = true;
		cv.notify_all();
	}

	mutex lock;
	std::condition_variable cv;
	string path;
	bool entered = false;
	bool cancelled = false;
	bool released = true;
	bool clear_first_interrupt = false;
	bool first_reader_only = false;
	std::function<bool()> should_pause;
	weak_ptr<ClientContext> reader_context;
	atomic<idx_t> open_handles {0};
};

struct GatedHandle final : public FileHandle {
	GatedHandle(FileSystem &fs, string path, FileOpenFlags flags, unique_ptr<FileHandle> inner_p,
	            optional_ptr<ClientContext> reader_p, ReadGate &gate_p)
	    : FileHandle(fs, std::move(path), flags), inner(std::move(inner_p)), reader(reader_p), gate(gate_p) {
		gate.open_handles++;
	}
	~GatedHandle() override {
		Close();
	}
	void Close() override {
		if (inner) {
			inner.reset();
			gate.open_handles--;
		}
	}
	unique_ptr<FileHandle> inner;
	optional_ptr<ClientContext> reader;
	ReadGate &gate;
};

class GatedFileSystem : public FileSystem {
public:
	explicit GatedFileSystem(string directory_p) : directory(std::move(directory_p)), local(FileSystem::CreateLocal()) {
	}
	string LocalPath(const string &path) {
		return directory + path.substr(string("gate://dataset").size());
	}
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override {
		CheckAccess(opener);
		return make_uniq<GatedHandle>(*this, path, flags, local->OpenFile(LocalPath(path), flags),
		                              FileOpener::TryGetClientContext(opener), gate);
	}
	void Read(FileHandle &handle, void *buffer, int64_t bytes, idx_t location) override {
		auto &h = handle.Cast<GatedHandle>();
		read_calls++;
		if (fail_reads) {
			throw IOException("Controlled timestamp read failure");
		}
		gate.Read(h.GetPath(), h.reader);
		h.inner->Read(buffer, bytes, location);
	}
	int64_t Read(FileHandle &handle, void *buffer, int64_t bytes) override {
		auto &h = handle.Cast<GatedHandle>();
		read_calls++;
		if (fail_reads) {
			throw IOException("Controlled timestamp read failure");
		}
		gate.Read(h.GetPath(), h.reader);
		return h.inner->Read(buffer, bytes);
	}
	int64_t GetFileSize(FileHandle &handle) override {
		return handle.Cast<GatedHandle>().inner->GetFileSize();
	}
	timestamp_t GetLastModifiedTime(FileHandle &handle) override {
		if (fixed_mtime) {
			return timestamp_t(123000000);
		}
		return local->GetLastModifiedTime(*handle.Cast<GatedHandle>().inner);
	}
	string GetVersionTag(FileHandle &handle) override {
		const auto revision = StringUtil::StartsWith(handle.GetPath(), "gate://dataset/data") ? version.load() : 0;
		return "nested-query-fixture-" + std::to_string(revision);
	}
	FileType GetFileType(FileHandle &handle) override {
		return handle.Cast<GatedHandle>().inner->GetType();
	}
	bool FileExists(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		return local->FileExists(LocalPath(path));
	}
	bool IsPipe(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
		return false;
	}
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override {
		CheckAccess(opener);
		auto files = local->Glob(LocalPath(path));
		for (auto &file : files) {
			file.path = "gate://dataset" + file.path.substr(directory.size());
		}
		return files;
	}
	void Seek(FileHandle &handle, idx_t position) override {
		handle.Cast<GatedHandle>().inner->Seek(position);
	}
	idx_t SeekPosition(FileHandle &handle) override {
		return handle.Cast<GatedHandle>().inner->SeekPosition();
	}
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}
	bool CanHandleFile(const string &path) override {
		return StringUtil::StartsWith(path, "gate://dataset/");
	}
	string GetName() const override {
		return "LeRobot controlled test reader";
	}
	void CheckAccess(optional_ptr<FileOpener> opener) {
		Value access;
		if (require_access && (!FileOpener::TryGetCurrentSetting(opener, "lerobot_test_access", access) ||
		                       access.IsNull() || access.GetValue<string>() != "fixture-session-access")) {
			throw IOException("Fixture session access denied");
		}
	}

	ReadGate gate;
	bool require_access = false;
	atomic<idx_t> read_calls {0};
	atomic<idx_t> version {0};
	atomic<bool> fail_reads {false};
	bool fixed_mtime = false;

private:
	string directory;
	unique_ptr<FileSystem> local;
};

enum class ReadStage { INFO, DATA_ROUTES, VIDEO_ROUTES, TIMESTAMPS };

struct GateFunctionInfo final : public TableFunctionInfo {
	explicit GateFunctionInfo(ReadGate &gate_p) : gate(gate_p) {
	}
	ReadGate &gate;
};

struct GateBindData final : public TableFunctionData {
	explicit GateBindData(ReadGate &gate_p) : gate(gate_p) {
	}
	ReadGate &gate;
};

struct GateScanState final : public GlobalTableFunctionState {
	idx_t position = 0;
};

void RegisterGateFunction(Connection &connection, ReadGate &gate) {
	TableFunction function(
	    "nested_gate", {},
	    [](ClientContext &context, TableFunctionInput &input, DataChunk &output) {
		    auto &state = input.global_state->Cast<GateScanState>();
		    if (state.position >= 100000000) {
			    return;
		    }
		    input.bind_data->Cast<GateBindData>().gate.Read("scan", &context);
		    auto data = FlatVector::GetData<int64_t>(output.data[0]);
		    for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
			    data[i] = state.position++;
		    }
		    output.SetCardinality(STANDARD_VECTOR_SIZE);
	    },
	    [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
	       vector<string> &names) -> unique_ptr<FunctionData> {
		    types.push_back(LogicalType::BIGINT);
		    names.push_back("value");
		    return make_uniq<GateBindData>(input.info->Cast<GateFunctionInfo>().gate);
	    },
	    [](ClientContext &, TableFunctionInitInput &) -> unique_ptr<GlobalTableFunctionState> {
		    return make_uniq<GateScanState>();
	    });
	function.function_info = make_shared_ptr<GateFunctionInfo>(gate);
	CreateTableFunctionInfo info(function);
	connection.BeginTransaction();
	Catalog::GetSystemCatalog(*connection.context).CreateTableFunction(*connection.context, info);
	connection.Commit();
}

struct NestedReadTest {
	explicit NestedReadTest(idx_t threads) {
		DBConfig config;
		config.options.load_extensions = false;
		config.options.maximum_threads = threads;
		config.AddExtensionOption("lerobot_test_access", "Controlled filesystem access", LogicalType::VARCHAR,
		                          Value("fixture-global-denied"));
		db = make_uniq<DuckDB>(nullptr, &config);
		for (const auto &extension : {"core_functions", "json", "parquet"}) {
			REQUIRE(ExtensionHelper::LoadExtension(*db, extension) == ExtensionLoadResult::LOADED_EXTENSION);
		}
		connection = make_uniq<Connection>(*db);
		// The video-route load follows a data-route load of the same Parquet.
		// Disable cached file contents so both tests exercise real reader I/O.
		Query("SET enable_external_file_cache = false");
		Query("COPY (SELECT 0 AS episode_index, 2 AS length, 0 AS \"data/chunk_index\", "
		      "0 AS \"data/file_index\", 0 AS \"videos/camera/chunk_index\", 0 AS \"videos/camera/file_index\", "
		      "0.0 AS \"videos/camera/from_timestamp\", 0.5 AS \"videos/camera/to_timestamp\") TO " +
		      Value(directory.path + "/meta/episodes/chunk-000/file-000.parquet").ToSQLString() + " (FORMAT PARQUET)");
		Query("COPY (SELECT 0::BIGINT AS episode_index, range::BIGINT AS frame_index, "
		      "range / 2.0 AS timestamp FROM range(2)) TO " +
		      Value(directory.path + "/data.parquet").ToSQLString() + " (FORMAT PARQUET)");
		const string info = R"({"codebase_version":"v3.0","data_path":"data.parquet","video_path":"video.mp4",
		"fps":2,"total_episodes":1,"total_frames":2,"total_tasks":0,
		"features":{"camera":{"dtype":"video","shape":[16,16,3]}}})";
		auto info_file = directory.fs->OpenFile(directory.path + "/meta/info.json",
		                                        FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		info_file->Write(const_cast<char *>(info.data()), info.size());
		info_file.reset();
		auto subsystem = make_uniq<GatedFileSystem>(directory.path);
		fs = subsystem.get();
		FileSystem::GetFileSystem(*db->instance).RegisterSubSystem(std::move(subsystem));
		RegisterGateFunction(*connection, fs->gate);
	}

	~NestedReadTest() {
		// Always unblock the worker before joining, including failed assertions.
		connection->Interrupt();
		fs->gate.Release();
		if (future.valid()) {
			future.wait();
		}
	}

	void Query(const string &sql) {
		auto result = connection->Query(sql);
		if (result->HasError()) {
			result->ThrowError();
		}
	}

	void Prepare(ReadStage stage) {
		if (stage == ReadStage::VIDEO_ROUTES || stage == ReadStage::TIMESTAMPS) {
			dataset = LerobotDatasetMetadata::Get(*connection->context, root, false);
		}
		if (stage == ReadStage::TIMESTAMPS) {
			videos = LerobotVideoMetadata::Get(*connection->context, root, false);
		}
		const string suffix = stage == ReadStage::INFO         ? "/meta/info.json"
		                      : stage == ReadStage::TIMESTAMPS ? "/data.parquet"
		                                                       : "/meta/episodes/chunk-000/file-000.parquet";
		fs->gate.Arm(root + suffix);
	}

	idx_t Read(ReadStage stage) {
		auto &context = *connection->context;
		switch (stage) {
		case ReadStage::INFO:
			return ReadLerobotDatasetInfo(context, root).total_frames;
		case ReadStage::DATA_ROUTES:
			return LerobotDatasetMetadata::Get(context, root, false)->GetEpisodeCount();
		case ReadStage::VIDEO_ROUTES:
			return LerobotVideoMetadata::Get(context, root, false)->GetVideoKeys().size();
		case ReadStage::TIMESTAMPS: {
			LerobotVideoTargetsBindData bind_data(dataset, videos, {}, {}, LerobotVideoOptions {});
			vector<LerobotDecodeTarget> targets {LerobotDecodeTarget(0, 0, 0, 0, 0),
			                                     LerobotDecodeTarget(0, 1, 0.5, 0.5, 0)};
			LerobotTimestampLookup lookup(context);
			auto values = ReadTargetTimestamps(context, bind_data, targets, lookup);
			if (values.at(LerobotFrameKey(0, 0)) != 0 || values.at(LerobotFrameKey(0, 1)) != 0.5) {
				throw InvalidInputException("Unexpected fixture timestamps");
			}
			return values.size();
		}
		default:
			throw InvalidInputException("Unknown test read stage");
		}
	}

	idx_t ConnectionCount() {
		return ConnectionManager::Get(*db->instance).GetConnectionCount();
	}

	TestDirectory directory;
	unique_ptr<DuckDB> db;
	unique_ptr<Connection> connection;
	GatedFileSystem *fs = nullptr;
	shared_ptr<LerobotDatasetMetadata> dataset;
	shared_ptr<LerobotVideoMetadata> videos;
	std::future<idx_t> future;
	const string root = "gate://dataset";
};

} // namespace

TEST_CASE("An S3 setting change during metadata loading cannot publish routes under the old cache key",
          "[lerobot][cache_scope]") {
	const bool video = GENERATE(false, true);
	const bool warm = GENERATE(false, true);
	const bool refresh = GENERATE(false, true);
	NestedReadTest test(1);
	DBConfig::GetConfig(*test.connection->context)
	    .AddExtensionOption("s3_endpoint", "Controlled storage identity", LogicalType::VARCHAR, Value("first"));
	Connection settings(*test.db);
	const string root = "s3://fixture-bucket/dataset";
	// Only episode routes differ. The same info.json file backs both endpoints.
	test.Query("COPY (SELECT * REPLACE (1 AS \"data/file_index\", 1 AS \"videos/camera/file_index\") FROM "
	           "read_parquet('" +
	           test.directory.path + "/meta/episodes/chunk-000/file-000.parquet')) TO '" + test.directory.path +
	           "/alternate.parquet' (FORMAT PARQUET)");
	auto info = test.directory.fs->OpenFile(test.directory.path + "/meta/info.json", FileFlags::FILE_FLAGS_READ);
	string json(info->GetFileSize(), '\0');
	info->Read(&json[0], json.size());
	info.reset();
	json = StringUtil::Replace(json, "data.parquet", "data-{file_index}.parquet");
	json = StringUtil::Replace(json, "video.mp4", "video-{file_index}.mp4");
	info = test.directory.fs->OpenFile(test.directory.path + "/meta/info.json",
	                                   FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
	info->Write(&json[0], json.size());
	info.reset();

	class SwitchingFileSystem final : public GatedFileSystem {
	public:
		explicit SwitchingFileSystem(const string &path) : GatedFileSystem(path) {
		}
		string Translate(const string &path) {
			return StringUtil::Replace(path, "s3://fixture-bucket/dataset", "gate://dataset");
		}
		bool CanHandleFile(const string &path) override {
			return StringUtil::StartsWith(path, "s3://fixture-bucket/dataset/");
		}
		unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
		                                optional_ptr<FileOpener> opener = nullptr) override {
			if ((repeat || !switched) && before_open && StringUtil::EndsWith(path, "/meta/info.json")) {
				switched = true;
				before_open();
			}
			const auto local = second && StringUtil::EndsWith(path, "/meta/episodes/chunk-000/file-000.parquet")
			                       ? "gate://dataset/alternate.parquet"
			                       : Translate(path);
			// Remote readers can request unaligned ranges with DIRECT_IO. Our
			// local backing file must not turn that hint into POSIX O_DIRECT.
			return GatedFileSystem::OpenFile(local, FileFlags::FILE_FLAGS_READ, opener);
		}
		vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override {
			auto files = GatedFileSystem::Glob(Translate(path), opener);
			for (auto &file : files) {
				file.path = StringUtil::Replace(file.path, "gate://dataset", "s3://fixture-bucket/dataset");
			}
			return files;
		}
		bool FileExists(const string &path, optional_ptr<FileOpener> opener = nullptr) override {
			return GatedFileSystem::FileExists(Translate(path), opener);
		}
		string GetName() const override {
			return "LeRobot controlled S3 endpoint switch";
		}
		bool second = false;
		bool switched = false;
		bool repeat = false;
		std::function<void()> before_open;
	};
	auto subsystem = make_uniq<SwitchingFileSystem>(test.directory.path);
	auto &fs = *subsystem;
	FileSystem::GetFileSystem(*test.db->instance).RegisterSubSystem(std::move(subsystem));
	auto read_path = [&](bool force_refresh) {
		if (video) {
			auto metadata = LerobotVideoMetadata::Get(*test.connection->context, root, force_refresh);
			return metadata->GetVideoFile(*metadata->FindRoute(0, "camera"));
		}
		return LerobotDatasetMetadata::Get(*test.connection->context, root, force_refresh)->GetDataFiles().at(0);
	};
	test.connection->BeginTransaction();
	if (warm) {
		REQUIRE(read_path(false) == root + (video ? "/video-0.mp4" : "/data-0.parquet"));
	}
	fs.before_open = [&] {
		auto result = settings.Query("SET GLOBAL s3_endpoint='second'");
		if (result->HasError()) {
			result->ThrowError();
		}
		fs.second = true;
	};
	REQUIRE(read_path(refresh) == root + (video ? "/video-1.mp4" : "/data-1.parquet"));
	REQUIRE_FALSE(settings.Query("SET GLOBAL s3_endpoint='first'")->HasError());
	fs.second = false;
	REQUIRE(read_path(false) == root + (video ? "/video-0.mp4" : "/data-0.parquet"));
	// Both identities are now cached. Repeated changes during fingerprint I/O
	// must exhaust the retry budget, then allow recovery when settings settle.
	idx_t switches = 0;
	fs.repeat = true;
	fs.before_open = [&] {
		fs.second = !fs.second;
		switches++;
		auto result = settings.Query(string("SET GLOBAL s3_endpoint='") + (fs.second ? "second" : "first") + "'");
		if (result->HasError()) {
			result->ThrowError();
		}
	};
	REQUIRE_THROWS_WITH(read_path(false), Catch::Contains("access configuration changed"));
	REQUIRE(switches == 2);
	fs.before_open = nullptr;
	REQUIRE(read_path(false) == root + (video ? "/video-0.mp4" : "/data-0.parquet"));
	// Revalidation performs I/O in the caller's context. Interrupting that read
	// must release its handle and leave the warmed cache usable afterwards.
	for (const auto *path :
	     {"gate://dataset/meta/info.json", "gate://dataset/meta/episodes/chunk-000/file-000.parquet"}) {
		struct ReleaseOnExit {
			ReadGate &gate;
			~ReleaseOnExit() {
				gate.Release();
			}
		} release {fs.gate};
		fs.gate.Arm(path);
		test.future = std::async(std::launch::async, [&] {
			read_path(false);
			return idx_t(1);
		});
		REQUIRE(fs.gate.Wait(&ReadGate::entered));
		REQUIRE(fs.gate.reader_context.lock().get() == test.connection->context.get());
		test.connection->Interrupt();
		REQUIRE(fs.gate.Wait(&ReadGate::cancelled));
		fs.gate.Release();
		REQUIRE_THROWS_AS(test.future.get(), InterruptException);
		REQUIRE(fs.gate.open_handles.load() == 0);
		test.connection->context->ClearInterrupt();
		REQUIRE(read_path(false) == root + (video ? "/video-0.mp4" : "/data-0.parquet"));
	}
	test.connection->Commit();
}

TEST_CASE("Nested native readers inherit session extension settings without changing other connections",
          "[lerobot][nested_query][reader_settings]") {
	NestedReadTest test(GENERATE(1, 4));
	test.fs->require_access = true;
	test.Query("SET SESSION lerobot_test_access='fixture-session-access'");
	REQUIRE(test.Read(ReadStage::INFO) == 2);
	REQUIRE(test.Read(ReadStage::DATA_ROUTES) == 1);
	REQUIRE(test.Read(ReadStage::VIDEO_ROUTES) == 1);
	test.dataset = LerobotDatasetMetadata::Get(*test.connection->context, test.root, false);
	test.videos = LerobotVideoMetadata::Get(*test.connection->context, test.root, false);
	REQUIRE(test.Read(ReadStage::TIMESTAMPS) == 2);
	{
		Connection other(*test.db);
		REQUIRE_THROWS_WITH(ReadLerobotDatasetInfo(*other.context, test.root),
		                    Catch::Contains("Fixture session access denied"));
		auto result = other.Query("SELECT current_setting('lerobot_test_access')");
		REQUIRE_FALSE(result->HasError());
		REQUIRE(result->GetValue(0, 0).GetValue<string>() == "fixture-global-denied");
	}
	test.Query("RESET lerobot_test_access");
	// A previously warmed metadata cache still checks access in this caller.
	REQUIRE_THROWS_WITH(test.Read(ReadStage::DATA_ROUTES), Catch::Contains("Fixture session access denied"));
	test.Query("SET SESSION lerobot_test_access='fixture-session-access'");
	REQUIRE(test.Read(ReadStage::TIMESTAMPS) == 2);
	REQUIRE(test.ConnectionCount() == 1);
	REQUIRE(test.fs->gate.open_handles.load() == 0);
}

TEST_CASE("Nested reader setting values preserve NULL and literal characters", "[lerobot][reader_settings]") {
	NestedReadTest test(1);
	for (const auto &value : {Value("quote'\\slash;\nfixture"), Value(LogicalType::VARCHAR)}) {
		test.Query("SET SESSION lerobot_test_access=" + value.ToSQLString());
		LerobotNestedQuery query(*test.connection->context, "SELECT current_setting('lerobot_test_access')");
		REQUIRE_FALSE(query.GetResult().HasError());
		auto chunk = query.Fetch();
		REQUIRE(chunk);
		REQUIRE(Value::NotDistinctFrom(chunk->GetValue(0, 0), value));
	}
	test.Query("RESET SESSION lerobot_test_access");
	test.Query("SET GLOBAL lerobot_test_access='new-global-value'");
	{
		// DuckDB RESET for extension options stores the registered default
		// as a session override. Preserve it even if the global value changes.
		LerobotNestedQuery query(*test.connection->context, "SELECT current_setting('lerobot_test_access')");
		REQUIRE_FALSE(query.GetResult().HasError());
		REQUIRE(query.Fetch()->GetValue(0, 0).GetValue<string>() == "fixture-global-denied");
	}
	Connection other(*test.db);
	LerobotNestedQuery query(*other.context, "SELECT current_setting('lerobot_test_access')");
	REQUIRE_FALSE(query.GetResult().HasError());
	REQUIRE(query.Fetch()->GetValue(0, 0).GetValue<string>() == "new-global-value");
}

TEST_CASE("Video producer reads Parquet with its caller's session access", "[lerobot][reader_settings]") {
	NestedReadTest test(GENERATE(1, 4));
	test.fs->require_access = true;
	test.Query("SET SESSION lerobot_test_access='fixture-session-access'");
	auto metadata = LerobotVideoMetadata::Get(*test.connection->context, test.root, false);
	vector<LerobotVideoRoute> routes {LerobotVideoRoute(0, 2, 0, 0, 0, 0, 0.0, 0.5)};
	vector<string> queries {
	    "SELECT episode_index, frame_index, timestamp FROM read_parquet('gate://dataset/data.parquet')"};
	LerobotVideoOptions options {};
	options.producer_threads = 1;
	options.target_buffer_size = 4;
	options.max_pending_targets = 4;
	LerobotVideoFramesBindData bind_data(metadata, routes, queries, false, options);
	auto state = make_shared_ptr<LerobotVideoProducerState>(bind_data);
	Executor executor(*test.connection->context);
	executor.Reset();
	{
		auto task = make_shared_ptr<LerobotVideoProducerTask>(executor, state, test.connection->context->db, 0, 1);
		REQUIRE(task->Execute(TaskExecutionMode::PROCESS_ALL) == TaskExecutionResult::TASK_FINISHED);
	}
	unique_ptr<LerobotDecodeBuffer> buffer;
	REQUIRE(state->ClaimBuffer(buffer) == LerobotBufferClaimResult::CLAIMED);
	REQUIRE(buffer->targets.size() == 2);
	REQUIRE(buffer->targets[0].frame_timestamp == 0);
	REQUIRE(buffer->targets[1].frame_timestamp == 0.5);
	REQUIRE(test.ConnectionCount() == 1);
	REQUIRE(test.fs->gate.open_handles.load() == 0);
	executor.CancelTasks();
}

static void CancelGatedRead(NestedReadTest &test) {
	if (!test.fs->gate.Wait(&ReadGate::entered)) {
		if (test.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			test.future.get();
		}
		FAIL("Native reader did not enter the controlled I/O gate");
	}
	REQUIRE(test.fs->gate.reader_context.lock() != test.connection->context);
	REQUIRE(test.ConnectionCount() == 2);
	test.connection->Interrupt();
	REQUIRE(test.fs->gate.Wait(&ReadGate::cancelled));
	REQUIRE(test.future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
	{
		Connection other(*test.db);
		auto result = other.Query("SELECT 42");
		REQUIRE_FALSE(result->HasError());
		REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 42);
	}
	test.fs->gate.Release();
	REQUIRE(test.future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	REQUIRE_THROWS_AS(test.future.get(), InterruptException);
	REQUIRE(test.ConnectionCount() == 1);
	REQUIRE(test.fs->gate.reader_context.expired());
	REQUIRE(test.fs->gate.open_handles.load() == 0);
}

TEST_CASE("Nested native reads propagate cancellation and finish their own I/O", "[lerobot][nested_query]") {
	const auto threads = GENERATE(1, 4);
	const auto stage =
	    GENERATE(ReadStage::INFO, ReadStage::DATA_ROUTES, ReadStage::VIDEO_ROUTES, ReadStage::TIMESTAMPS);
	CAPTURE(threads, static_cast<int>(stage));
	NestedReadTest test(threads);
	test.Prepare(stage);
	test.future = std::async(std::launch::async, [&] { return test.Read(stage); });
	// Cancellation must not detach the query or free its borrowed resources
	// while its own read is still running.
	CancelGatedRead(test);
	if (stage == ReadStage::DATA_ROUTES) {
		REQUIRE_FALSE(LerobotDatasetMetadata::Peek(*test.connection->context, test.root));
	} else if (stage == ReadStage::VIDEO_ROUTES) {
		REQUIRE_FALSE(LerobotVideoMetadata::Peek(*test.connection->context, test.root));
	}
	test.connection->context->ClearInterrupt();
	const idx_t expected = stage == ReadStage::INFO || stage == ReadStage::TIMESTAMPS ? 2 : 1;
	REQUIRE(test.Read(stage) == expected);
	test.Query("SELECT 42");
}

TEST_CASE("Nested streaming fetch remains cancellable after query construction", "[lerobot][nested_query]") {
	const auto threads = GENERATE(1, 4);
	const auto reset_interrupt = GENERATE(false, true);
	CAPTURE(threads, reset_interrupt);
	NestedReadTest test(threads);
	test.fs->gate.clear_first_interrupt = reset_interrupt;
	test.future = std::async(std::launch::async, [&]() -> idx_t {
		LerobotNestedQuery query(*test.connection->context, "SELECT * FROM nested_gate()", true);
		if (query.GetResult().type != QueryResultType::STREAM_RESULT) {
			throw InvalidInputException("Expected streaming query result");
		}
		// Arm only after SendQuery returns. Once its initial buffer is consumed,
		// Fetch must execute the controlled scan and receive the interrupt.
		test.fs->gate.Arm("scan");
		while (query.Fetch()) {
		}
		return 0;
	});
	CancelGatedRead(test);
	test.connection->context->ClearInterrupt();
	test.Query("SELECT 42");
}

TEST_CASE("Nested query scopes clean up before cancellation, on errors and on early exit", "[lerobot][nested_query]") {
	const auto stream = GENERATE(false, true);
	NestedReadTest test(1);
	test.connection->Interrupt();
	REQUIRE_THROWS_AS(LerobotNestedQuery(*test.connection->context, "SELECT * FROM nested_gate()", stream),
	                  InterruptException);
	REQUIRE(test.ConnectionCount() == 1);
	test.connection->context->ClearInterrupt();
	for (idx_t i = 0; i < 20; i++) {
		{
			LerobotNestedQuery query(*test.connection->context, "SELECT * FROM range(1000000)", stream);
			REQUIRE_FALSE(query.GetResult().HasError());
			REQUIRE(query.Fetch()->size() > 0);
			// Deliberately leave most of the result unconsumed.
		}
		REQUIRE(test.ConnectionCount() == 1);
		{
			LerobotNestedQuery query(*test.connection->context, "SELECT * FROM missing_nested_test_table", stream);
			REQUIRE(query.GetResult().HasError());
			REQUIRE(query.GetResult().GetErrorType() == ExceptionType::CATALOG);
		}
		REQUIRE(test.ConnectionCount() == 1);
	}
	test.Query("SELECT 42");
}

TEST_CASE("Shared cancellation monitoring isolates simultaneous nested connections", "[lerobot][nested_query]") {
	NestedReadTest test(4);
	test.Prepare(ReadStage::INFO);
	test.future = std::async(std::launch::async, [&] { return test.Read(ReadStage::INFO); });
	REQUIRE(test.fs->gate.Wait(&ReadGate::entered));
	{
		Connection other(*test.db);
		LerobotNestedQuery query(*other.context, "SELECT * FROM range(1000000)", true);
		REQUIRE(test.ConnectionCount() == 4);
		test.connection->Interrupt();
		REQUIRE(test.fs->gate.Wait(&ReadGate::cancelled));
		REQUIRE(query.Fetch()->GetValue(0, 0).GetValue<int64_t>() == 0);
		REQUIRE_FALSE(query.GetResult().HasError());
		REQUIRE_FALSE(other.context->IsInterrupted());
		test.fs->gate.Release();
		REQUIRE(test.future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		REQUIRE_THROWS_AS(test.future.get(), InterruptException);
		REQUIRE(test.ConnectionCount() == 3);
		REQUIRE(query.Fetch()->size() > 0);
	}
	REQUIRE(test.ConnectionCount() == 1);
	REQUIRE(test.fs->gate.reader_context.expired());
	test.connection->context->ClearInterrupt();
	test.Query("SELECT 42");
}

static void WriteTimestampRows(NestedReadTest &test, const string &select, const string &name = "data.parquet") {
	test.Query("COPY (" + select + ") TO " + Value(test.directory.path + "/" + name).ToSQLString() +
	           " (FORMAT PARQUET, COMPRESSION UNCOMPRESSED)");
}

static void SameTimestampMatches(const vector<LerobotTimestampMatch> &expected,
                                 const vector<LerobotTimestampMatch> &actual) {
	REQUIRE(actual.size() == expected.size());
	for (idx_t i = 0; i < expected.size(); i++) {
		CAPTURE(i);
		REQUIRE(actual[i].count == expected[i].count);
		if (expected[i].count != 1) {
			continue; // A non-unique match is rejected regardless of its value.
		}
		REQUIRE(actual[i].has_timestamp == expected[i].has_timestamp);
		if (expected[i].has_timestamp) {
			if (std::isnan(expected[i].timestamp)) {
				REQUIRE(std::isnan(actual[i].timestamp));
			} else {
				REQUIRE(actual[i].timestamp == expected[i].timestamp);
			}
		}
	}
}

TEST_CASE("Empty timestamp shards preserve missing matches after index construction", "[lerobot][timestamp_lookup]") {
	NestedReadTest test(1);
	WriteTimestampRows(test,
	                   "SELECT 0::BIGINT episode_index, 0::BIGINT frame_index, 0.0::DOUBLE AS timestamp WHERE false");
	auto &context = *test.connection->context;
	LerobotTimestampLookup lookup(context, 32 * 1024 * 1024);
	for (idx_t i = 0; i < 5; i++) {
		REQUIRE(lookup.Lookup(context, {test.root + "/data.parquet"}, {{0, 0}})[0].count == 0);
	}
	REQUIRE(lookup.GetStats().index_loads == 1);
	REQUIRE(lookup.GetStats().bytes == 0);
	REQUIRE(test.fs->gate.open_handles.load() == 0);
}

TEST_CASE("Small timestamp requests stay filtered even with one-row batches", "[lerobot][timestamp_lookup]") {
	NestedReadTest test(1);
	auto &context = *test.connection->context;
	LerobotTimestampLookup lookup(context);
	const vector<string> files {test.root + "/data.parquet"};
	for (idx_t i = 0; i < 10; i++) {
		REQUIRE(lookup.Lookup(context, files, {{0, 1}})[0].timestamp == 0.5);
	}
	REQUIRE(lookup.GetStats().index_loads == 0);
	REQUIRE(lookup.GetStats().bytes == 0);
	REQUIRE(lookup.GetStats().queries == 10);
	// A batch may contain many duplicate logical targets with one unique key.
	REQUIRE(lookup.Lookup(context, files, {{0, 1}}, 4096)[0].timestamp == 0.5);
	REQUIRE(lookup.GetStats().index_loads == 1);
}

TEST_CASE("Index construction does not validate unrequested timestamp casts", "[lerobot][timestamp_lookup]") {
	NestedReadTest test(1);
	WriteTimestampRows(test, "SELECT * FROM (VALUES (0::BIGINT,0::BIGINT,'0.5'), (0,1,'broken')) "
	                         "t(episode_index,frame_index,timestamp)");
	auto &context = *test.connection->context;
	const vector<string> files {test.root + "/data.parquet"};
	LerobotTimestampLookup native(context, 0);
	REQUIRE(native.Lookup(context, files, {{0, 0}})[0].timestamp == 0.5);
	LerobotTimestampLookup indexed(context, 1024);
	for (idx_t i = 0; i < 5; i++) {
		REQUIRE(indexed.Lookup(context, files, {{0, 0}})[0].timestamp == 0.5);
	}
	REQUIRE_THROWS_WITH(indexed.Lookup(context, files, {{0, 1}}), Catch::Contains("Could not convert string"));
	REQUIRE(indexed.GetStats().bytes == 0);
	REQUIRE(test.fs->gate.open_handles.load() == 0);
}

TEST_CASE("Repeated timestamp batches reuse one bounded index and stop reading columns",
          "[lerobot][timestamp_lookup]") {
	NestedReadTest test(1);
	auto &context = *test.connection->context;
	const vector<string> files {test.root + "/data.parquet"};
	const vector<LerobotTimestampKey> keys {{0, 0}, {0, 1}};
	LerobotTimestampLookup lookup(context, 32 * 1024 * 1024);
	for (idx_t i = 0; i < 3; i++) {
		auto matches = lookup.Lookup(context, files, keys);
		REQUIRE(matches[0].count == 1);
		REQUIRE(matches[1].timestamp == 0.5);
	}
	const auto reads = test.fs->read_calls.load();
	const auto queries = lookup.GetStats().queries;
	REQUIRE(lookup.GetStats().index_loads == 1);
	REQUIRE(queries == 4); // two filtered queries, footer count, index scan
	for (idx_t i = 0; i < 20; i++) {
		REQUIRE(lookup.Lookup(context, files, keys)[1].timestamp == 0.5);
	}
	REQUIRE(test.fs->read_calls.load() == reads);
	REQUIRE(lookup.GetStats().queries == queries);
	REQUIRE(lookup.GetStats().index_hits == 20);
	REQUIRE(lookup.GetStats().indexed_rows == 2);
	REQUIRE(lookup.GetStats().bytes > 0);
	REQUIRE(lookup.GetStats().peak_bytes <= 32 * 1024 * 1024);
	REQUIRE(test.fs->gate.open_handles.load() == 0);
}

TEST_CASE("Timestamp index preserves native matching for unordered, null and duplicate rows",
          "[lerobot][timestamp_lookup]") {
	NestedReadTest test(4);
	WriteTimestampRows(test, "SELECT * FROM (VALUES (1099511627776::BIGINT,1099511627776::BIGINT,0.125::DOUBLE), "
	                         "(0,5,0.25),(0,5,0.5),(0,4,'Infinity'::DOUBLE),(0,3,'NaN'::DOUBLE),"
	                         "(0,2,-1.0),(0,1,NULL),(0,0,0.0),(NULL,0,1.0),(0,NULL,1.0)) "
	                         "t(episode_index,frame_index,timestamp)");
	auto &context = *test.connection->context;
	const vector<string> files {test.root + "/data.parquet"};
	const vector<LerobotTimestampKey> keys {{0, 0}, {0, 1}, {0, 2}, {0, 3},
	                                        {0, 4}, {0, 5}, {0, 6}, {1099511627776LL, 1099511627776LL}};
	LerobotTimestampLookup native(context, 0);
	const auto expected = native.Lookup(context, files, keys);
	LerobotTimestampLookup indexed(context, 32 * 1024 * 1024);
	for (idx_t i = 0; i < 5; i++) {
		SameTimestampMatches(expected, indexed.Lookup(context, files, keys));
	}
	REQUIRE(expected[5].count == 2);
	REQUIRE(expected[6].count == 0);
	REQUIRE(indexed.GetStats().index_loads == 1);
	REQUIRE(indexed.GetStats().indexed_rows == 8);
	REQUIRE(test.fs->gate.open_handles.load() == 0);
}

TEST_CASE("Cached and uncached shards still detect misplaced duplicate frames", "[lerobot][timestamp_lookup]") {
	NestedReadTest test(1);
	WriteTimestampRows(test, "SELECT 0::BIGINT episode_index, 1::BIGINT frame_index, 0.75::DOUBLE AS timestamp",
	                   "data-other.parquet");
	auto &context = *test.connection->context;
	const vector<LerobotTimestampKey> keys {{0, 0}, {0, 1}};
	LerobotTimestampLookup lookup(context, 32 * 1024 * 1024);
	for (idx_t i = 0; i < 3; i++) {
		lookup.Lookup(context, {test.root + "/data.parquet"}, keys);
	}
	for (idx_t i = 0; i < 5; i++) {
		auto matches = lookup.Lookup(context, {test.root + "/data.parquet", test.root + "/data-other.parquet"}, keys);
		REQUIRE(matches[0].count == 1);
		REQUIRE(matches[1].count == 2);
	}
	REQUIRE(lookup.GetStats().index_loads == 2);
}

TEST_CASE("Timestamp indexes respect the byte cap, evict, and bypass oversized shards", "[lerobot][timestamp_lookup]") {
	const auto bytes = GENERATE(0, 1, 128);
	NestedReadTest test(1);
	WriteTimestampRows(test,
	                   "SELECT 1::BIGINT episode_index, i::BIGINT frame_index, i / 2.0 AS timestamp FROM range(4) t(i)",
	                   "data-other.parquet");
	auto &context = *test.connection->context;
	LerobotTimestampLookup lookup(context, bytes);
	for (idx_t round = 0; round < 3; round++) {
		for (idx_t i = 0; i < 5; i++) {
			REQUIRE(lookup.Lookup(context, {test.root + "/data.parquet"}, {{0, 1}})[0].timestamp == 0.5);
		}
		for (idx_t i = 0; i < 5; i++) {
			REQUIRE(lookup.Lookup(context, {test.root + "/data-other.parquet"}, {{1, 3}})[0].timestamp == 1.5);
		}
	}
	const auto stats = lookup.GetStats();
	REQUIRE(stats.peak_bytes <= static_cast<idx_t>(bytes));
	if (bytes == 128) {
		REQUIRE(stats.index_loads == 2);
		REQUIRE(stats.evictions > 0);
	} else {
		REQUIRE(stats.index_loads == 0);
		REQUIRE(stats.queries == (bytes ? 32 : 30));
	}
	REQUIRE(test.fs->gate.open_handles.load() == 0);
}

TEST_CASE("Independent shard replacement invalidates timestamps without an info.json change",
          "[lerobot][timestamp_lookup]") {
	NestedReadTest test(1);
	test.fs->fixed_mtime = true;
	const auto select = "SELECT 0::BIGINT episode_index, i::BIGINT frame_index, i * ";
	WriteTimestampRows(test, select + string("0.5 AS timestamp FROM range(2) t(i)"));
	auto &context = *test.connection->context;
	const vector<string> files {test.root + "/data.parquet"};
	const auto old_size =
	    test.directory.fs->OpenFile(test.directory.path + "/data.parquet", FileFlags::FILE_FLAGS_READ)->GetFileSize();
	LerobotTimestampLookup lookup(context, 32 * 1024 * 1024);
	for (idx_t i = 0; i < 4; i++) {
		REQUIRE(lookup.Lookup(context, files, {{0, 1}})[0].timestamp == 0.5);
	}
	WriteTimestampRows(test, select + string("1.5 AS timestamp FROM range(2) t(i)"));
	REQUIRE(
	    test.directory.fs->OpenFile(test.directory.path + "/data.parquet", FileFlags::FILE_FLAGS_READ)->GetFileSize() ==
	    old_size);
	test.fs->version++;
	for (idx_t i = 0; i < 4; i++) {
		REQUIRE(lookup.Lookup(context, files, {{0, 1}})[0].timestamp == 1.5);
	}
	REQUIRE(lookup.GetStats().index_loads == 2);
	LerobotTimestampLookup next_query(context);
	REQUIRE(next_query.Lookup(context, files, {{0, 1}})[0].timestamp == 1.5);
	REQUIRE(next_query.GetStats().index_loads == 0);
}

TEST_CASE("Timestamp index load failure releases handles and can retry", "[lerobot][timestamp_lookup]") {
	NestedReadTest test(1);
	auto &context = *test.connection->context;
	LerobotTimestampLookup lookup(context, 32 * 1024 * 1024);
	const vector<string> files {test.root + "/data.parquet"};
	for (idx_t i = 0; i < 2; i++) {
		lookup.Lookup(context, files, {{0, 1}});
	}
	test.fs->fail_reads = true;
	REQUIRE_THROWS_WITH(lookup.Lookup(context, files, {{0, 1}}), Catch::Contains("Controlled timestamp read failure"));
	REQUIRE(lookup.GetStats().bytes == 0);
	REQUIRE(test.fs->gate.open_handles.load() == 0);
	test.fs->fail_reads = false;
	REQUIRE(lookup.Lookup(context, files, {{0, 1}})[0].timestamp == 0.5);
	REQUIRE(lookup.GetStats().index_loads == 0);
	LerobotTimestampLookup next_query(context, 1024);
	for (idx_t i = 0; i < 3; i++) {
		REQUIRE(next_query.Lookup(context, files, {{0, 1}})[0].timestamp == 0.5);
	}
	REQUIRE(next_query.GetStats().index_loads == 1);
	test.Query("SELECT 42");
}

// Unblock and join before destroying the locally owned lookup, including when
// a REQUIRE fails while the native query is still paused.
struct TimestampTaskScope {
	explicit TimestampTaskScope(NestedReadTest &test_p) : test(test_p) {
	}
	~TimestampTaskScope() {
		if (test.future.valid()) {
			test.connection->Interrupt();
			test.fs->gate.Release();
			test.future.wait();
		}
	}
	NestedReadTest &test;
};

TEST_CASE("Concurrent timestamp lookup does not wait for an index loader or share its cancellation",
          "[lerobot][timestamp_lookup]") {
	const auto cancel = GENERATE(false, true);
	const auto separate_shard = GENERATE(false, true);
	NestedReadTest test(4);
	auto &context = *test.connection->context;
	LerobotTimestampLookup lookup(context, 64);
	TimestampTaskScope task_scope(test);
	const vector<string> files {test.root + "/data.parquet"};
	for (idx_t i = 0; i < 2; i++) {
		lookup.Lookup(context, files, {{0, 1}});
	}
	const vector<string> other_files {test.root + (separate_shard ? "/data-other.parquet" : "/data.parquet")};
	if (separate_shard) {
		WriteTimestampRows(
		    test, "SELECT 0::BIGINT episode_index, i::BIGINT frame_index, i * 1.5 AS timestamp FROM range(2) t(i)",
		    "data-other.parquet");
		for (idx_t i = 0; i < 2; i++) {
			lookup.Lookup(context, other_files, {{0, 1}});
		}
	}
	test.fs->gate.first_reader_only = true;
	test.fs->gate.should_pause = [&] {
		return lookup.GetStats().bytes > 0;
	};
	test.fs->gate.Arm(files[0]);
	test.future = std::async(std::launch::async, [&]() -> idx_t {
		return lookup.Lookup(context, files, {{0, 1}})[0].count;
	});
	REQUIRE(test.fs->gate.Wait(&ReadGate::entered));
	Connection other(*test.db);
	auto other_future = std::async(std::launch::async, [&] {
		return lookup.Lookup(*other.context, other_files, {{0, 1}})[0].timestamp;
	});
	const auto other_status = other_future.wait_for(std::chrono::seconds(5));
	if (other_status != std::future_status::ready) {
		test.fs->gate.Release(); // make failed assertions safe to unwind
	}
	REQUIRE(other_status == std::future_status::ready);
	REQUIRE(other_future.get() == (separate_shard ? 1.5 : 0.5));
	REQUIRE(lookup.GetStats().peak_bytes <= 64);
	REQUIRE(lookup.GetStats().index_loads == 0);
	REQUIRE(test.future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
	if (cancel) {
		test.connection->Interrupt();
		REQUIRE(test.fs->gate.Wait(&ReadGate::cancelled));
	}
	test.fs->gate.Release();
	REQUIRE(test.future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	if (cancel) {
		REQUIRE_THROWS_AS(test.future.get(), InterruptException);
		REQUIRE(lookup.GetStats().bytes == 0);
		test.connection->context->ClearInterrupt();
	} else {
		REQUIRE(test.future.get() == 1);
	}
	REQUIRE(lookup.Lookup(context, files, {{0, 1}})[0].timestamp == 0.5);
	REQUIRE(lookup.GetStats().index_loads == 1);
	REQUIRE(test.fs->gate.open_handles.load() == 0);
}

TEST_CASE("Shard generation changed during index loading is never published", "[lerobot][timestamp_lookup]") {
	NestedReadTest test(1);
	auto &context = *test.connection->context;
	LerobotTimestampLookup lookup(context, 32 * 1024 * 1024);
	TimestampTaskScope task_scope(test);
	const vector<string> files {test.root + "/data.parquet"};
	for (idx_t i = 0; i < 2; i++) {
		lookup.Lookup(context, files, {{0, 1}});
	}
	test.fs->gate.should_pause = [&] {
		return lookup.GetStats().bytes > 0;
	};
	test.fs->gate.Arm(files[0]);
	test.future = std::async(std::launch::async, [&]() -> idx_t {
		return lookup.Lookup(context, files, {{0, 1}})[0].count;
	});
	REQUIRE(test.fs->gate.Wait(&ReadGate::entered));
	test.fs->version++;
	test.fs->gate.Release();
	REQUIRE(test.future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	REQUIRE(test.future.get() == 1);
	REQUIRE(lookup.GetStats().index_loads == 0);
	REQUIRE(lookup.GetStats().bytes == 0);
	for (idx_t i = 0; i < 3; i++) {
		REQUIRE(lookup.Lookup(context, files, {{0, 1}})[0].timestamp == 0.5);
	}
	REQUIRE(lookup.GetStats().index_loads == 1);
}
