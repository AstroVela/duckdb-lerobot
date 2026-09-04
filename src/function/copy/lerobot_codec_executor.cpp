#include "function/lerobot_codec_executor.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>

namespace duckdb {

namespace {

static const char *LEROBOT_CODEC_EXECUTOR_CACHE_KEY = "__duckdb_lerobot_codec_executor";

struct LerobotCodecBatchState {
	LerobotCodecBatchState(idx_t job_count, idx_t max_workers_p, idx_t codec_thread_budget_p,
	                       idx_t host_thread_budget_p)
	    : results(job_count), max_workers(max_workers_p), codec_thread_budget(codec_thread_budget_p),
	      host_thread_budget(host_thread_budget_p), remaining_jobs(job_count) {
	}

	mutex result_lock;
	std::condition_variable result_cv;
	vector<LerobotCodecResult> results;
	std::exception_ptr error;
	idx_t completed_jobs = 0;
	atomic<bool> cancelled {false};

	//! Protected by LerobotCodecExecutor::Impl::lock.
	idx_t max_workers;
	idx_t codec_thread_budget;
	idx_t host_thread_budget;
	idx_t active_workers = 0;
	idx_t active_codec_threads = 0;
	idx_t remaining_jobs;
};

struct LerobotQueuedCodecJob {
	shared_ptr<LerobotCodecBatchState> batch;
	LerobotCodecJob job;
	idx_t result_index = 0;
	idx_t codec_threads = 0;
	FileSystem *fs = nullptr;
};

} // namespace

struct LerobotCodecExecutor::Impl {
	~Impl() {
		{
			lock_guard<mutex> guard(lock);
			stopping = true;
			for (auto &batch_ref : batches) {
				auto batch = batch_ref.lock();
				if (batch) {
					batch->cancelled = true;
				}
			}
		}
		work_cv.notify_all();
		for (auto &worker : workers) {
			if (worker->joinable()) {
				worker->join();
			}
		}
	}

	idx_t GlobalCodecCapacityLocked() {
		idx_t capacity = NumericLimits<idx_t>::Maximum();
		bool found_batch = false;
		for (auto entry = batches.begin(); entry != batches.end();) {
			auto batch = entry->lock();
			if (!batch || batch->remaining_jobs == 0) {
				entry = batches.erase(entry);
				continue;
			}
			found_batch = true;
			capacity = MinValue(capacity, batch->host_thread_budget);
			entry++;
		}
		return found_batch ? capacity : 1;
	}

	idx_t DesiredWorkerCountLocked() {
		idx_t desired_workers = 0;
		const auto capacity = GlobalCodecCapacityLocked();
		for (auto &batch_ref : batches) {
			auto batch = batch_ref.lock();
			if (!batch || batch->remaining_jobs == 0) {
				continue;
			}
			desired_workers += MinValue(batch->max_workers, batch->remaining_jobs);
			if (desired_workers >= capacity) {
				return capacity;
			}
		}
		return desired_workers;
	}

	void EnsureWorkersLocked() {
		const auto desired_workers = DesiredWorkerCountLocked();
		if (workers.size() >= desired_workers) {
			return;
		}
		workers.reserve(desired_workers);
		while (workers.size() < desired_workers) {
			workers.push_back(make_uniq<thread>([this] { WorkerLoop(); }));
		}
	}

	bool TryTakeJobLocked(LerobotQueuedCodecJob &result, bool &reserved_budget) {
		const auto global_capacity = GlobalCodecCapacityLocked();
		for (auto entry = queue.begin(); entry != queue.end(); entry++) {
			auto &batch = *entry->batch;
			if (batch.cancelled.load()) {
				result = std::move(*entry);
				queue.erase(entry);
				reserved_budget = false;
				return true;
			}
			if (batch.active_workers >= batch.max_workers ||
			    batch.active_codec_threads + entry->codec_threads > batch.codec_thread_budget ||
			    active_codec_threads + entry->codec_threads > global_capacity) {
				continue;
			}
			batch.active_workers++;
			batch.active_codec_threads += entry->codec_threads;
			active_codec_threads += entry->codec_threads;
			result = std::move(*entry);
			queue.erase(entry);
			reserved_budget = true;
			return true;
		}
		return false;
	}

	void CompleteJob(LerobotQueuedCodecJob &job, bool reserved_budget, LerobotEncodedVideoInfo encoded,
	                 std::exception_ptr error) {
		auto &batch = *job.batch;
		if (error && !batch.cancelled.exchange(true)) {
			lock_guard<mutex> result_guard(batch.result_lock);
			batch.error = error;
		}
		{
			lock_guard<mutex> result_guard(batch.result_lock);
			if (!error && !batch.cancelled.load()) {
				auto &result = batch.results[job.result_index];
				result.feature_index = job.job.feature_index;
				result.output_path = std::move(job.job.output_path);
				result.encoded = std::move(encoded);
			}
			batch.completed_jobs++;
		}
		{
			lock_guard<mutex> guard(lock);
			if (reserved_budget) {
				D_ASSERT(batch.active_workers > 0);
				D_ASSERT(batch.active_codec_threads >= job.codec_threads);
				D_ASSERT(active_codec_threads >= job.codec_threads);
				batch.active_workers--;
				batch.active_codec_threads -= job.codec_threads;
				active_codec_threads -= job.codec_threads;
			}
			D_ASSERT(batch.remaining_jobs > 0);
			batch.remaining_jobs--;
		}
		batch.result_cv.notify_all();
		work_cv.notify_all();
	}

	void WorkerLoop() {
		while (true) {
			LerobotQueuedCodecJob job;
			bool reserved_budget = false;
			{
				unique_lock<mutex> guard(lock);
				while (!TryTakeJobLocked(job, reserved_budget)) {
					if (stopping) {
						return;
					}
					work_cv.wait(guard);
				}
			}

			LerobotEncodedVideoInfo encoded;
			std::exception_ptr error;
			if (!job.batch->cancelled.load()) {
				try {
					job.job.options.cancelled = job.batch->cancelled;
					encoded = LerobotVisualWriter::EncodeVideo(*job.fs, job.job.output_path, job.job.raw_frames_path,
					                                           job.job.frame_count, job.job.options);
				} catch (...) {
					error = std::current_exception();
				}
			}
			CompleteJob(job, reserved_budget, std::move(encoded), error);
		}
	}

	vector<LerobotCodecResult> Execute(ClientContext &context, FileSystem &fs, vector<LerobotCodecJob> jobs,
	                                   idx_t max_workers, idx_t codec_thread_budget) {
		if (jobs.empty()) {
			return {};
		}
		const auto host_thread_budget = MaxValue<idx_t>(1, context.db->NumberOfThreads());
		if (max_workers == 0 || codec_thread_budget == 0 || codec_thread_budget > host_thread_budget) {
			throw InternalException("Invalid LeRobot codec executor budget");
		}
		max_workers = MinValue(max_workers, jobs.size());
		max_workers = MinValue(max_workers, codec_thread_budget);
		const auto base_codec_threads = codec_thread_budget / max_workers;
		const auto extra_codec_threads = codec_thread_budget % max_workers;
		auto batch =
		    make_shared_ptr<LerobotCodecBatchState>(jobs.size(), max_workers, codec_thread_budget, host_thread_budget);

		{
			lock_guard<mutex> guard(lock);
			if (stopping) {
				throw InternalException("LeRobot codec executor is stopping");
			}
			batches.push_back(batch);
			try {
				for (idx_t job_index = 0; job_index < jobs.size(); job_index++) {
					const auto worker_slot = job_index % max_workers;
					const auto codec_threads = base_codec_threads + (worker_slot < extra_codec_threads ? 1 : 0);
					jobs[job_index].options.encoder_threads = optional_idx(codec_threads);
					LerobotQueuedCodecJob queued_job;
					queued_job.batch = batch;
					queued_job.job = std::move(jobs[job_index]);
					queued_job.result_index = job_index;
					queued_job.codec_threads = codec_threads;
					queued_job.fs = &fs;
					queue.push_back(std::move(queued_job));
				}
				EnsureWorkersLocked();
			} catch (...) {
				batch->cancelled = true;
				for (auto entry = queue.begin(); entry != queue.end();) {
					if (entry->batch == batch) {
						entry = queue.erase(entry);
					} else {
						entry++;
					}
				}
				D_ASSERT(!batches.empty());
				batches.pop_back();
				throw;
			}
		}
		work_cv.notify_all();

		bool interrupted = false;
		unique_lock<mutex> result_guard(batch->result_lock);
		while (batch->completed_jobs != batch->results.size()) {
			if (context.IsInterrupted()) {
				interrupted = true;
				batch->cancelled = true;
				work_cv.notify_all();
			}
			batch->result_cv.wait_for(result_guard, std::chrono::milliseconds(10));
		}
		auto error = batch->error;
		result_guard.unlock();
		if (interrupted || context.IsInterrupted()) {
			throw InterruptException();
		}
		if (error) {
			std::rethrow_exception(error);
		}
		return std::move(batch->results);
	}

	mutex lock;
	std::condition_variable work_cv;
	std::deque<LerobotQueuedCodecJob> queue;
	vector<unique_ptr<thread>> workers;
	vector<weak_ptr<LerobotCodecBatchState>> batches;
	idx_t active_codec_threads = 0;
	bool stopping = false;
};

LerobotCodecExecutor::LerobotCodecExecutor() : impl(make_uniq<Impl>()) {
}

LerobotCodecExecutor::~LerobotCodecExecutor() {
}

string LerobotCodecExecutor::ObjectType() {
	return "lerobot_codec_executor";
}

string LerobotCodecExecutor::GetObjectType() {
	return ObjectType();
}

optional_idx LerobotCodecExecutor::GetEstimatedCacheMemory() const {
	//! Keep the worker pool alive until its DatabaseInstance is destroyed.
	return optional_idx();
}

shared_ptr<LerobotCodecExecutor> LerobotCodecExecutor::Get(ClientContext &context) {
	auto result =
	    ObjectCache::GetObjectCache(context).GetOrCreate<LerobotCodecExecutor>(LEROBOT_CODEC_EXECUTOR_CACHE_KEY);
	if (!result) {
		throw InternalException("LeRobot codec executor cache key has an incompatible object type");
	}
	return result;
}

vector<LerobotCodecResult> LerobotCodecExecutor::Execute(ClientContext &context, FileSystem &fs,
                                                         vector<LerobotCodecJob> jobs, idx_t max_workers,
                                                         idx_t codec_thread_budget) {
	return impl->Execute(context, fs, std::move(jobs), max_workers, codec_thread_budget);
}

} // namespace duckdb
