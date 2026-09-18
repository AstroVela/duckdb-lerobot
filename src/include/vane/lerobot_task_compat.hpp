#pragma once

#ifdef LEROBOT_VANE_DISTRIBUTED
#include "duckdb/parallel/executor_task.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include <atomic>

namespace duckdb {
namespace {
// The upstream producer has one outstanding wake per blocked execution. Its
// queue removes that wake before invoking it. Capture Vane's epoch while the
// producer is RUNNING: asking ExecutorTask for it after descheduling violates
// Vane's interrupt contract (and trips its debug assertion).
class LerobotVaneProducerTask : public ::duckdb::ExecutorTask {
public:
	using ::duckdb::ExecutorTask::ExecutorTask;
	uint64_t CurrentInterruptEpoch() const override {
		return producer_epoch.load(std::memory_order_acquire);
	}
	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) final {
		producer_epoch.store(::duckdb::ExecutorTask::CurrentInterruptEpoch(), std::memory_order_release);
		return ExecuteProducerTask(mode);
	}
	virtual TaskExecutionResult ExecuteProducerTask(TaskExecutionMode mode) = 0;

private:
	std::atomic<uint64_t> producer_epoch {0};
};

} // namespace
} // namespace duckdb
#endif
