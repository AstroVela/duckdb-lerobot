//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_temporal.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

static constexpr double LEROBOT_DEFAULT_TEMPORAL_TOLERANCE_SECONDS = 1e-4;

struct LerobotTemporalDelta {
	double timestamp;
	int64_t frame_offset;
};

vector<LerobotTemporalDelta> GetLerobotTemporalDeltas(TableFunctionBindInput &input, int64_t fps, double tolerance,
                                                      const char *function_name);

} // namespace duckdb
