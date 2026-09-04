//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_video_options.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

enum class LerobotDepthOutputUnit : uint8_t { METERS, MILLIMETERS };

struct LerobotVideoOptions {
	double tolerance;
	double cluster_gap;
	int32_t width;
	int32_t height;
	idx_t output_batch_size;
	idx_t target_buffer_size;
	idx_t max_cached_decoders;
	idx_t decode_threads;
	idx_t producer_threads;
	idx_t max_pending_targets;
	idx_t max_output_bytes;
	int32_t codec_threads;
	LerobotDepthOutputUnit depth_output_unit;
};

LerobotVideoOptions GetLerobotVideoOptions(TableFunctionBindInput &input, const char *function_name);

void AddLerobotVideoOptionParameters(TableFunction &function, bool include_video_keys = true,
                                     bool source_function = true);

} // namespace duckdb
