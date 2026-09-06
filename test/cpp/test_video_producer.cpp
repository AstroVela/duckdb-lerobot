#define CATCH_CONFIG_MAIN
#include "catch.hpp"

// These task/state types are private to the video source. Including the actual
// implementation lets the tests retain the state after DuckDB destroys tasks,
// without exporting test hooks or maintaining a copy of the scheduler logic.
#include "../../src/function/video/lerobot_video_frames.cpp"

#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection_manager.hpp"

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
