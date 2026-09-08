#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "function/lerobot_codec_executor.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>

using namespace duckdb;

namespace {

const auto TEST_TIMEOUT = std::chrono::seconds(5);
using BatchFuture = std::future<vector<LerobotCodecResult>>;

// The test writer uses the supplied FileSystem as its control state. No files
// or codecs are needed, and the production executor has no test-only hooks.
class ControlledEncoder : public FileSystem {
public:
	explicit ControlledEncoder(idx_t job_count) : jobs(job_count) {
	}

	string GetName() const override {
		return "controlled_encoder";
	}

	void Block(idx_t id, bool fail = false) {
		std::lock_guard<std::mutex> guard(lock);
		jobs[id].released = false;
		jobs[id].fail = fail;
	}

	void Release(idx_t id) {
		std::lock_guard<std::mutex> guard(lock);
		jobs[id].released = true;
		cv.notify_all();
	}

	void ReleaseAll() {
		std::lock_guard<std::mutex> guard(lock);
		for (auto &job : jobs) {
			job.released = true;
		}
		cv.notify_all();
	}

	bool WaitStarted(idx_t id) {
		std::unique_lock<std::mutex> guard(lock);
		return cv.wait_for(guard, TEST_TIMEOUT, [&] { return jobs[id].started; });
	}

	bool WaitCancelled(idx_t id) {
		std::unique_lock<std::mutex> guard(lock);
		return cv.wait_for(guard, TEST_TIMEOUT, [&] { return jobs[id].cancel_seen; });
	}

	bool WaitFinished(idx_t id) {
		std::unique_lock<std::mutex> guard(lock);
		return cv.wait_for(guard, TEST_TIMEOUT, [&] { return jobs[id].finished; });
	}

	bool Started(idx_t id) {
		std::lock_guard<std::mutex> guard(lock);
		return jobs[id].started;
	}

	idx_t ThreadBudget(idx_t id) {
		std::lock_guard<std::mutex> guard(lock);
		return jobs[id].thread_budget;
	}

	idx_t ActiveJobs() {
		std::lock_guard<std::mutex> guard(lock);
		return active_jobs;
	}

	idx_t PeakJobs() {
		std::lock_guard<std::mutex> guard(lock);
		return peak_jobs;
	}

	idx_t PeakCodecThreads() {
		std::lock_guard<std::mutex> guard(lock);
		return peak_codec_threads;
	}

	LerobotEncodedVideoInfo Encode(const string &raw_path, idx_t frame_count,
	                               const LerobotVideoEncodeOptions &options) {
		const auto id = std::stoull(raw_path);
		std::unique_lock<std::mutex> guard(lock);
		auto &job = jobs.at(id);
		job.started = true;
		job.thread_budget = options.encoder_threads.GetIndex();
		active_jobs++;
		active_codec_threads += job.thread_budget;
		peak_jobs = MaxValue(peak_jobs, active_jobs);
		peak_codec_threads = MaxValue(peak_codec_threads, active_codec_threads);
		cv.notify_all();
		while (!job.released) {
			if (options.cancelled->load()) {
				job.cancel_seen = true;
				cv.notify_all();
			}
			cv.wait_for(guard, std::chrono::milliseconds(1));
		}
		job.finished = true;
		active_jobs--;
		active_codec_threads -= job.thread_budget;
		cv.notify_all();
		if (job.fail) {
			throw InvalidInputException("injected codec failure");
		}
		if (options.cancelled->load()) {
			throw InterruptException();
		}
		LerobotEncodedVideoInfo result;
		result.frame_count = frame_count;
		result.codec = "test";
		return result;
	}

private:
	struct Job {
		bool released = true;
		bool fail = false;
		bool started = false;
		bool cancel_seen = false;
		bool finished = false;
		idx_t thread_budget = 0;
	};
	std::mutex lock;
	std::condition_variable cv;
	vector<Job> jobs;
	idx_t active_jobs = 0;
	idx_t active_codec_threads = 0;
	idx_t peak_jobs = 0;
	idx_t peak_codec_threads = 0;
};

enum class BatchExit { SUCCESS, CANCEL, FAIL };

struct ExecutorTest {
	explicit ExecutorTest(idx_t threads, idx_t job_count) : encoder(job_count) {
		DBConfig config;
		config.options.load_extensions = false;
		config.options.maximum_threads = threads;
		db = make_uniq<DuckDB>(nullptr, &config);
	}

	~ExecutorTest() {
		// Release every gate before waiting for any batch, including on assertion
		// failure: the old cancellation bug waits for another batch's encoder.
		encoder.ReleaseAll();
		for (auto &batch : batches) {
			if (batch->valid()) {
				batch->wait();
			}
		}
	}

	Connection &Connect() {
		connections.push_back(make_uniq<Connection>(*db));
		return *connections.back();
	}

	BatchFuture &Run(Connection &connection, const vector<idx_t> &ids, idx_t workers = 1, idx_t budget = 1) {
		vector<LerobotCodecJob> jobs;
		for (auto id : ids) {
			LerobotCodecJob job;
			job.feature_index = id;
			job.output_path = "encoded_" + std::to_string(id);
			job.raw_frames_path = std::to_string(id);
			job.frame_count = id + 1;
			jobs.push_back(std::move(job));
		}
		auto executor = LerobotCodecExecutor::Get(*connection.context);
		batches.push_back(make_uniq<BatchFuture>(
		    std::async(std::launch::async, [this, &connection, executor, jobs, workers, budget]() mutable {
			    return executor->Execute(*connection.context, encoder, std::move(jobs), workers, budget);
		    })));
		return *batches.back();
	}

	unique_ptr<DuckDB> db;
	vector<unique_ptr<Connection>> connections;
	ControlledEncoder encoder;
	vector<unique_ptr<BatchFuture>> batches;
};

bool Ready(BatchFuture &batch) {
	return batch.wait_for(TEST_TIMEOUT) == std::future_status::ready;
}

bool Pending(BatchFuture &batch) {
	return batch.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
}

} // namespace

namespace duckdb {

LerobotEncodedVideoInfo LerobotVisualWriter::EncodeVideo(FileSystem &fs, const string &, const string &raw_frames_path,
                                                         idx_t frame_count, const LerobotVideoEncodeOptions &options) {
	return dynamic_cast<ControlledEncoder &>(fs).Encode(raw_frames_path, frame_count, options);
}

} // namespace duckdb

TEST_CASE("Queued codec cancellation does not wait for another COPY", "[lerobot][codec_executor]") {
	ExecutorTest test(1, 6);
	auto &first = test.Connect();
	auto &second = test.Connect();
	test.encoder.Block(0);
	auto &busy = test.Run(first, {0});
	REQUIRE(test.encoder.WaitStarted(0));

	// The only worker is inside the first encoder. Pre-interrupt the second
	// context so its coordinator sees cancellation as soon as it enqueues.
	second.Interrupt();
	auto &cancelled = test.Run(second, {1, 2, 3});
	REQUIRE(Ready(cancelled));
	REQUIRE_THROWS_AS(cancelled.get(), InterruptException);
	REQUIRE(Pending(busy));
	for (idx_t id = 1; id <= 3; id++) {
		REQUIRE_FALSE(test.encoder.Started(id));
	}

	test.encoder.Release(0);
	REQUIRE(Ready(busy));
	REQUIRE(busy.get().size() == 1);
	second.context->ClearInterrupt();
	auto &reused = test.Run(second, {4, 5});
	REQUIRE(Ready(reused));
	REQUIRE(reused.get().size() == 2);
}

TEST_CASE("Codec cancellation waits for its running encoder", "[lerobot][codec_executor]") {
	ExecutorTest test(1, 3);
	auto &connection = test.Connect();
	test.encoder.Block(0);
	auto &batch = test.Run(connection, {0, 1, 2});
	REQUIRE(test.encoder.WaitStarted(0));
	connection.Interrupt();
	REQUIRE(test.encoder.WaitCancelled(0));
	REQUIRE(Pending(batch));
	test.encoder.Release(0);
	REQUIRE(Ready(batch));
	REQUIRE_THROWS_AS(batch.get(), InterruptException);
	REQUIRE_FALSE(test.encoder.Started(1));
	REQUIRE_FALSE(test.encoder.Started(2));
}

TEST_CASE("Codec failure preserves the error and waits only for its own encoders", "[lerobot][codec_executor]") {
	ExecutorTest test(3, 5);
	auto &first = test.Connect();
	auto &second = test.Connect();
	test.encoder.Block(0);
	test.encoder.Block(1, true);
	test.encoder.Block(2);
	auto &busy = test.Run(first, {0});
	REQUIRE(test.encoder.WaitStarted(0));
	auto &failed = test.Run(second, {1, 2, 3}, 2, 2);
	REQUIRE(test.encoder.WaitStarted(1));
	REQUIRE(test.encoder.WaitStarted(2));
	test.encoder.Release(1);
	REQUIRE(test.encoder.WaitCancelled(2));
	REQUIRE(Pending(failed));
	test.encoder.Release(2);
	REQUIRE(Ready(failed));
	REQUIRE_THROWS_WITH(failed.get(), Catch::Contains("injected codec failure"));
	REQUIRE_FALSE(test.encoder.Started(3));
	REQUIRE(Pending(busy));
	auto &reused = test.Run(second, {4});
	REQUIRE(Ready(reused));
	REQUIRE(reused.get().size() == 1);
	test.encoder.Release(0);
	REQUIRE(Ready(busy));
	REQUIRE(busy.get().size() == 1);
}

TEST_CASE("Codec results retain input order and thread budgets", "[lerobot][codec_executor]") {
	ExecutorTest test(3, 3);
	auto &connection = test.Connect();
	test.encoder.Block(2);
	test.encoder.Block(0);
	auto &batch = test.Run(connection, {2, 0, 1}, 2, 3);
	REQUIRE(test.encoder.WaitStarted(2));
	REQUIRE(test.encoder.WaitStarted(0));
	test.encoder.Release(0);
	REQUIRE(test.encoder.WaitFinished(0));
	REQUIRE(Pending(batch));
	test.encoder.Release(2);
	REQUIRE(Ready(batch));
	const auto results = batch.get();
	REQUIRE(results.size() == 3);
	const vector<idx_t> ids {2, 0, 1};
	for (idx_t i = 0; i < ids.size(); i++) {
		REQUIRE(results[i].feature_index == ids[i]);
		REQUIRE(results[i].output_path == "encoded_" + std::to_string(ids[i]));
		REQUIRE(results[i].encoded.frame_count == ids[i] + 1);
		REQUIRE(results[i].encoded.codec == "test");
	}
	REQUIRE(test.encoder.ThreadBudget(2) == 2);
	REQUIRE(test.encoder.ThreadBudget(0) == 1);
	REQUIRE(test.encoder.ThreadBudget(1) == 2);
}

TEST_CASE("Codec workers and coordinator can discard cancelled jobs concurrently", "[lerobot][codec_executor]") {
	const idx_t iterations = 32;
	const idx_t jobs_per_batch = 32;
	ExecutorTest test(2, iterations * jobs_per_batch);
	auto &connection = test.Connect();
	for (idx_t iteration = 0; iteration < iterations; iteration++) {
		const auto first_id = iteration * jobs_per_batch;
		vector<idx_t> ids;
		for (idx_t id = first_id; id < first_id + jobs_per_batch; id++) {
			ids.push_back(id);
		}
		test.encoder.Block(first_id);
		test.encoder.Block(first_id + 1);
		auto &batch = test.Run(connection, ids, 2, 2);
		REQUIRE(test.encoder.WaitStarted(first_id));
		REQUIRE(test.encoder.WaitStarted(first_id + 1));
		connection.Interrupt();
		REQUIRE(test.encoder.WaitCancelled(first_id));
		test.encoder.Release(first_id);
		test.encoder.Release(first_id + 1);
		REQUIRE(Ready(batch));
		REQUIRE_THROWS_AS(batch.get(), InterruptException);
		for (idx_t id = first_id + 2; id < first_id + jobs_per_batch; id++) {
			REQUIRE_FALSE(test.encoder.Started(id));
		}
		connection.context->ClearInterrupt();
	}
}

TEST_CASE("Codec pool grows for capacity released by an overlapping batch", "[lerobot][codec_executor]") {
	const auto high_budget = GENERATE(4, 6);
	const auto exit = GENERATE(BatchExit::SUCCESS, BatchExit::CANCEL, BatchExit::FAIL);
	CAPTURE(high_budget, static_cast<int>(exit));
	ExecutorTest test(2, 6);
	auto &first = test.Connect();
	auto &second = test.Connect();
	auto &settings = test.Connect();
	test.encoder.Block(0, exit == BatchExit::FAIL);
	auto &low = test.Run(first, {0, 5});
	REQUIRE(test.encoder.WaitStarted(0));

	// The first batch retains its host budget of 2. A new batch can use the
	// larger setting once the first finishes, but both currently share 2.
	auto setting = settings.Query("SET threads = " + std::to_string(high_budget));
	REQUIRE_FALSE(setting->HasError());
	for (idx_t id = 1; id <= 4; id++) {
		test.encoder.Block(id);
	}
	auto &high = test.Run(second, {1, 2, 3, 4}, 4, high_budget);
	// One spare codec thread lets this job start. That proves the whole high
	// batch has been enqueued and its pool sizing has completed before we
	// release the low batch. No sleeps or executor testing hooks are needed.
	const idx_t first_high_id = high_budget == 4 ? 1 : 3;
	REQUIRE(test.encoder.WaitStarted(first_high_id));
	REQUIRE(test.encoder.ActiveJobs() == 2);
	REQUIRE(test.encoder.PeakCodecThreads() == 2);
	for (idx_t id = 1; id <= 4; id++) {
		if (id != first_high_id) {
			REQUIRE_FALSE(test.encoder.Started(id));
		}
	}
	REQUIRE_FALSE(test.encoder.Started(5));
	if (exit == BatchExit::CANCEL) {
		first.Interrupt();
		REQUIRE(test.encoder.WaitCancelled(0));
		REQUIRE(Pending(low));
	}
	test.encoder.Release(0);
	REQUIRE(Ready(low));
	if (exit == BatchExit::SUCCESS) {
		REQUIRE(low.get().size() == 2);
	} else if (exit == BatchExit::CANCEL) {
		REQUIRE_THROWS_AS(low.get(), InterruptException);
		REQUIRE_FALSE(test.encoder.Started(5));
	} else {
		REQUIRE_THROWS_WITH(low.get(), Catch::Contains("injected codec failure"));
		REQUIRE_FALSE(test.encoder.Started(5));
	}

	// All four encoders must run concurrently without another Execute call.
	// Holding each gate closed prevents serial completion from passing this.
	for (idx_t id = 1; id <= 4; id++) {
		REQUIRE(test.encoder.WaitStarted(id));
		const idx_t expected_threads = high_budget == 6 && id <= 2 ? 2 : 1;
		REQUIRE(test.encoder.ThreadBudget(id) == expected_threads);
	}
	REQUIRE(test.encoder.ActiveJobs() == 4);
	REQUIRE(test.encoder.PeakJobs() == 4);
	REQUIRE(test.encoder.PeakCodecThreads() == static_cast<idx_t>(high_budget));
	REQUIRE(Pending(high));
	test.encoder.ReleaseAll();
	REQUIRE(Ready(high));
	const auto results = high.get();
	REQUIRE(results.size() == 4);
	for (idx_t index = 0; index < results.size(); index++) {
		REQUIRE(results[index].feature_index == index + 1);
		REQUIRE(results[index].encoded.frame_count == index + 2);
	}
	REQUIRE(test.encoder.ActiveJobs() == 0);
	first.context->ClearInterrupt();
	REQUIRE_FALSE(first.Query("SELECT 42")->HasError());
}
