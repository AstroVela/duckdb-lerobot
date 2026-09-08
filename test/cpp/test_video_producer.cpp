#define CATCH_CONFIG_MAIN
#include "catch.hpp"

// These task/state types are private to the video source. Including the actual
// implementation lets the tests retain the state after DuckDB destroys tasks,
// without exporting test hooks or maintaining a copy of the scheduler logic.
#include "../../src/function/video/lerobot_video_frames.cpp"

#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"

#ifdef LEROBOT_HAVE_FFMPEG
#include "duckdb/common/types/uuid.hpp"
#include <fstream>
#include <iterator>
#endif

using namespace duckdb;

namespace {

const string ONE_FRAME = "SELECT 0::BIGINT, 0::BIGINT, 0.0::DOUBLE";
const string TWO_FRAMES = "SELECT * FROM (VALUES (0::BIGINT, 0::BIGINT, 0.0::DOUBLE), "
                          "(0::BIGINT, 1::BIGINT, 0.5::DOUBLE)) frames(episode_index, frame_index, timestamp)";

struct ProducerTest {
	ProducerTest(idx_t threads, const vector<string> &queries, idx_t max_pending = 1) {
		DBConfig db_config;
		db_config.options.load_extensions = false;
		db_config.options.maximum_threads = threads;
		db = make_uniq<DuckDB>(nullptr, &db_config);
		connection = make_uniq<Connection>(*db);
		executor = make_uniq<Executor>(*connection->context);
		executor->Reset();

		vector<LerobotVideoRoute> routes {LerobotVideoRoute(0, 2, 0, 0, 0, 0, 0.0, 0.5)};
		auto metadata = make_shared_ptr<LerobotVideoMetadata>(
		    "producer_test", "video.mp4", 2, vector<string> {"camera"}, vector<LerobotVideoFeatureMetadata>(1), routes,
		    vector<string> {"video.mp4"}, LerobotDatasetMetadata::FileFingerprint(0, timestamp_t(0), ""));
		metadata_ref = metadata;
		LerobotVideoOptions options {};
		options.producer_threads = queries.size();
		options.max_pending_targets = max_pending;
		options.target_buffer_size = 4;
		options.cluster_gap = 1;
		LerobotVideoFramesBindData bind_data(metadata, routes, queries, false, options);
		state = make_shared_ptr<LerobotVideoProducerState>(bind_data);
		// bind_data and the local metadata reference intentionally go away here.
	}

	~ProducerTest() {
		executor->CancelTasks();
	}

	shared_ptr<LerobotVideoProducerTask> Task(idx_t index = 0) {
		return make_shared_ptr<LerobotVideoProducerTask>(*executor, state, connection->context->db, index,
		                                                 state->ProducerCount());
	}

	idx_t ConnectionCount() {
		return ConnectionManager::Get(*db->instance).GetConnectionCount();
	}

	void CheckHealthy() {
		connection->context->ClearInterrupt();
		auto result = connection->Query("SELECT 42");
		REQUIRE_FALSE(result->HasError());
		REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 42);
	}

	unique_ptr<DuckDB> db;
	unique_ptr<Connection> connection;
	unique_ptr<Executor> executor;
	shared_ptr<LerobotVideoProducerState> state;
	weak_ptr<LerobotVideoMetadata> metadata_ref;
};

} // namespace

#ifdef LEROBOT_HAVE_FFMPEG
namespace {

string MOVUInt32(uint32_t value) {
	string bytes(4, '\0');
	for (idx_t i = 0; i < 4; i++) {
		bytes[i] = static_cast<char>(value >> (24 - 8 * i));
	}
	return bytes;
}

uint32_t MOVSize(const string &bytes, idx_t offset) {
	REQUIRE(offset + 8 <= bytes.size());
	uint32_t size = 0;
	for (idx_t i = 0; i < 4; i++) {
		size = (size << 8) | static_cast<uint8_t>(bytes[offset + i]);
	}
	REQUIRE(size >= 8);
	REQUIRE(size <= bytes.size() - offset);
	return size;
}

string MOVAtom(const string &type, const string &payload) {
	return MOVUInt32(8 + payload.size()) + type + payload;
}

string ReplaceMOVReference(const string &bytes, const string &reference, idx_t &replaced) {
	string result;
	for (idx_t offset = 0; offset < bytes.size();) {
		const auto size = MOVSize(bytes, offset);
		const auto type = bytes.substr(offset + 4, 4);
		if (type == "dref") {
			result += MOVAtom(type, MOVUInt32(0) + MOVUInt32(1) + reference);
			replaced++;
		} else if (type == "moov" || type == "trak" || type == "mdia" || type == "minf" || type == "dinf") {
			result += MOVAtom(type, ReplaceMOVReference(bytes.substr(offset + 8, size - 8), reference, replaced));
		} else {
			result += bytes.substr(offset, size);
		}
		offset += size;
	}
	return result;
}

struct ReferenceMovie {
	ReferenceMovie() : fs(FileSystem::CreateLocal()) {
		std::ifstream input(LEROBOT_TEST_DATA_DIR "/lerobot/long-20701.mp4", std::ios::binary);
		REQUIRE(input.good());
		const string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		// QuickTime alias record: filename, one parent directory, then the
		// external media filename. Only generated test media is referenced.
		const string filename = "outside.mp4";
		string alias(150, '\0');
		alias[7] = 2; // alias record version
		alias[50] = static_cast<char>(filename.size());
		alias.replace(51, filename.size(), filename);
		alias[131] = 2; // levels up from the dataset
		alias[133] = 1; // levels down to the referenced file
		alias += string("\0\2\0", 3) + static_cast<char>(filename.size()) + filename;
		if (filename.size() % 2) {
			alias += '\0';
		}
		alias += string("\xff\xff\0\0", 4); // end of alias extra data
		alias[4] = static_cast<char>(alias.size() >> 8);
		alias[5] = static_cast<char>(alias.size());
		const auto reference = MOVAtom("alis", MOVUInt32(0) + alias);
		string movie, moov;
		idx_t replaced = 0;
		for (idx_t offset = 0; offset < source.size();) {
			const auto size = MOVSize(source, offset);
			if (source.substr(offset + 4, 4) == "moov") {
				REQUIRE(moov.empty());
				moov = ReplaceMOVReference(source.substr(offset, size), reference, replaced);
				// Preserve every sample's byte offset by replacing the original
				// moov with equal-sized padding and appending the new moov.
				movie += MOVAtom("free", string(size - 8, '\0'));
			} else if (source.substr(offset + 4, 4) == "mdat") {
				// The reference movie contains no usable local sample data.
				movie += MOVAtom("mdat", string(size - 8, '\0'));
			} else {
				movie += source.substr(offset, size);
			}
			offset += size;
		}
		REQUIRE(replaced == 1);
		movie += moov;
		root = fs->JoinPath(FileSystem::GetWorkingDirectory(),
		                    "build/video_reference_" + UUID::ToString(UUID::GenerateRandomUUID()));
		fs->CreateDirectoriesRecursive(root + "/dataset");
		Write(root + "/outside.mp4", source);
		path = root + "/dataset/video.mp4";
		Write(path, movie);
	}
	~ReferenceMovie() {
		fs->RemoveDirectory(root);
	}
	void Write(const string &path, const string &bytes) {
		auto file = fs->OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW);
		file->Write(const_cast<char *>(bytes.data()), bytes.size());
		file->Close();
	}
	unique_ptr<FileSystem> fs;
	string root;
	string path;
};

struct MOVInput {
	~MOVInput() {
		avformat_close_input(&format);
		if (io) {
			av_freep(&io->buffer);
			avio_context_free(&io);
		}
		av_dict_free(&options);
		av_packet_free(&packet);
	}
	AVFormatContext *format = avformat_alloc_context();
	AVIOContext *io = nullptr;
	AVDictionary *options = nullptr;
	AVPacket *packet = av_packet_alloc();
};

} // namespace

TEST_CASE("Decoder I/O rejects valid MOV external data references", "[lerobot][video_decode_io]") {
	ReferenceMovie movie;
	DuckDB db(nullptr);
	Connection connection(db);
	const auto mov = av_find_input_format("mov");
	REQUIRE(mov != nullptr);
	for (bool guarded : {false, true}) {
		CAPTURE(guarded);
		LerobotVideoDecodeMetrics metrics;
		DuckDBAVIOState state(*connection.context, movie.path, metrics);
		MOVInput input;
		REQUIRE(input.format != nullptr);
		REQUIRE(input.packet != nullptr);
		if (guarded) {
			input.io = state.Attach(*input.format);
		}
		// MOV disables external tracks by default. Enable them only in this
		// test so FFmpeg must reach the production io_open callback, rather
		// than skipping the alias before it can exercise the I/O boundary.
		REQUIRE(av_dict_set(&input.options, "enable_drefs", "1", 0) == 0);
		REQUIRE(av_dict_set(&input.options, "use_absolute_path", "1", 0) == 0);
		const auto status = avformat_open_input(&input.format, movie.path.c_str(), mov, &input.options);
		if (guarded) {
			REQUIRE(state.secondary_open_denied);
			REQUIRE_THROWS_AS(state.ThrowIOError(movie.path), PermissionException);
			REQUIRE_THROWS_WITH(state.ThrowIOError(movie.path),
			                    Catch::Contains("cannot open external media references"));
		} else {
			// Independent positive control: the same MOV must resolve the
			// external track and yield a real packet without our callback.
			REQUIRE(status >= 0);
			REQUIRE(avformat_find_stream_info(input.format, nullptr) >= 0);
			REQUIRE(av_read_frame(input.format, input.packet) >= 0);
			REQUIRE(input.packet->size > 0);
			REQUIRE(std::any_of(input.packet->data, input.packet->data + input.packet->size,
			                    [](uint8_t byte) { return byte != 0; }));
		}
	}
}
#endif

TEST_CASE("Cancelling blocked video producers completes their lifetime", "[lerobot][video_producer]") {
	const auto threads = GENERATE(1, 4);
	ProducerTest test(threads, {TWO_FRAMES, TWO_FRAMES});
	vector<weak_ptr<LerobotVideoProducerTask>> tasks;
	for (idx_t i = 0; i < 2; i++) {
		auto task = test.Task(i);
		REQUIRE(task->Execute(TaskExecutionMode::PROCESS_ALL) == TaskExecutionResult::TASK_BLOCKED);
		task->Deschedule();
		tasks.push_back(task);
	}
	REQUIRE(test.state->GetMetrics().producer_waits.load() == 2);
	REQUIRE(test.ConnectionCount() == 3);
	REQUIRE(test.state->ProducerCount() == 2);

	// CancelTasks drops the last references while holding executor_lock.
	// Destruction must not try to reschedule another blocked producer.
	test.connection->Interrupt();
	test.executor->CancelTasks();
	for (auto &task : tasks) {
		REQUIRE(task.expired());
	}
	REQUIRE(test.state->ProducerCount() == 0);
	REQUIRE(test.state->ShouldStop());
	REQUIRE(test.ConnectionCount() == 1);
	unique_ptr<LerobotDecodeBuffer> buffer;
	REQUIRE(test.state->ClaimBuffer(buffer) == LerobotBufferClaimResult::FINISHED);
	REQUIRE_FALSE(buffer);
	test.CheckHealthy();
}

TEST_CASE("Abandoning a video producer discards partial buffers", "[lerobot][video_producer]") {
	ProducerTest test(1, {ONE_FRAME}, 4);
	auto task = test.Task();
	idx_t position = 0;
	vector<LerobotDecodeTarget> targets {LerobotDecodeTarget(0, 0, 0.0, 0.0, 0)};
	REQUIRE(test.state->QueueTargets(targets, position, task) == LerobotTargetQueueResult::QUEUED);
	REQUIRE_FALSE(test.state->CanConsumerProgress());
	task.reset();
	REQUIRE(test.state->ProducerCount() == 0);
	REQUIRE(test.state->ShouldStop());
	unique_ptr<LerobotDecodeBuffer> buffer;
	REQUIRE(test.state->ClaimBuffer(buffer) == LerobotBufferClaimResult::FINISHED);
	REQUIRE_FALSE(buffer);
	test.CheckHealthy();
}

TEST_CASE("Normal video producer completion publishes the tail exactly once", "[lerobot][video_producer]") {
	ProducerTest test(1, {ONE_FRAME, ONE_FRAME}, 4);
	auto first = test.Task(0);
	auto second = test.Task(1);
	REQUIRE(first->Execute(TaskExecutionMode::PROCESS_ALL) == TaskExecutionResult::TASK_FINISHED);
	first.reset();
	REQUIRE(test.state->ProducerCount() == 1);
	REQUIRE_FALSE(test.state->CanConsumerProgress());
	REQUIRE(second->Execute(TaskExecutionMode::PROCESS_ALL) == TaskExecutionResult::TASK_FINISHED);
	second.reset();
	REQUIRE(test.state->ProducerCount() == 0);
	REQUIRE_FALSE(test.state->ShouldStop());
	REQUIRE(test.ConnectionCount() == 1);
	unique_ptr<LerobotDecodeBuffer> buffer;
	REQUIRE(test.state->ClaimBuffer(buffer) == LerobotBufferClaimResult::CLAIMED);
	REQUIRE(buffer->targets.size() == 2);
	test.state->FinishBuffer(buffer->shard_index);
	buffer.reset();
	REQUIRE(test.state->ClaimBuffer(buffer) == LerobotBufferClaimResult::FINISHED);
	test.CheckHealthy();
}

TEST_CASE("Cancelling video production retains consumer buffer ownership", "[lerobot][video_producer]") {
	ProducerTest test(1, {ONE_FRAME}, 4);
	auto task = test.Task();
	idx_t position = 0;
	vector<LerobotDecodeTarget> targets(4, LerobotDecodeTarget(0, 0, 0.0, 0.0, 0));
	REQUIRE(test.state->QueueTargets(targets, position, task) == LerobotTargetQueueResult::QUEUED);
	unique_ptr<LerobotDecodeBuffer> buffer;
	REQUIRE(test.state->ClaimBuffer(buffer) == LerobotBufferClaimResult::CLAIMED);
	task.reset();
	REQUIRE(test.state->ProducerCount() == 0);
	REQUIRE(buffer->targets.size() == 4);
	test.state->FinishBuffer(buffer->shard_index);
	buffer.reset();
	REQUIRE(test.state->ClaimBuffer(buffer) == LerobotBufferClaimResult::FINISHED);
	test.CheckHealthy();
}

TEST_CASE("Video producer errors survive cleanup", "[lerobot][video_producer]") {
	const string invalid_frames = "SELECT * FROM (VALUES (0::BIGINT, 0::BIGINT, 0.0::DOUBLE), "
	                              "(0::BIGINT, -1::BIGINT, 0.5::DOUBLE)) frames";
	ProducerTest test(1, {invalid_frames}, 4);
	auto task = test.Task();
	REQUIRE(task->Execute(TaskExecutionMode::PROCESS_ALL) == TaskExecutionResult::TASK_FINISHED);
	task.reset();
	REQUIRE(test.state->ProducerCount() == 0);
	REQUIRE(test.state->ShouldStop());
	REQUIRE(test.ConnectionCount() == 1);
	unique_ptr<LerobotDecodeBuffer> buffer;
	REQUIRE_THROWS_WITH(test.state->ClaimBuffer(buffer), Catch::Contains("Invalid LeRobot frame alignment metadata"));
	test.CheckHealthy();
}

TEST_CASE("Video producer ownership survives bind data and ends with the task", "[lerobot][video_producer]") {
	ProducerTest test(1, {ONE_FRAME});
	auto task = test.Task();
	weak_ptr<LerobotVideoProducerState> state = test.state;
	test.state.reset();
	REQUIRE_FALSE(state.expired());
	REQUIRE_FALSE(test.metadata_ref.expired());
	task.reset();
	REQUIRE(state.expired());
	REQUIRE(test.metadata_ref.expired());
	REQUIRE(test.ConnectionCount() == 1);
	test.CheckHealthy();
}
