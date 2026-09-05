//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_copy_options.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/copy_function.hpp"
#include "function/lerobot_video_writer.hpp"

namespace duckdb {

struct LerobotCopyConfig {
	idx_t fps;
	idx_t chunks_size;
	double data_file_size_mb;
	double video_file_size_mb;
	idx_t metadata_buffer_size;
	idx_t max_visual_frame_bytes;
	idx_t video_workers;
	idx_t encoder_threads;
	string robot_type;
	bool has_robot_type;
	string features_json;
	LerobotVideoEncodingConfig encoding;
};

LerobotCopyConfig ParseLerobotCopyRequiredConfig(const CopyFunctionBindInput &input);

void ParseLerobotCopyOptionalConfig(ClientContext &context, const CopyFunctionBindInput &input,
                                    LerobotCopyConfig &config);

void LerobotCopyOptionDefinitions(ClientContext &context, CopyOptionsInput &input);

} // namespace duckdb
