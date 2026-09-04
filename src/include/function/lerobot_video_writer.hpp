//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_video_writer.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {

class FileSystem;

enum class LerobotRawVisualType : uint8_t { RGB24, DEPTH_UINT16, DEPTH_FLOAT32 };

struct LerobotVideoEncodeOptions {
	idx_t width = 0;
	idx_t height = 0;
	idx_t fps = 0;
	optional_idx encoder_threads;
	LerobotRawVisualType raw_type = LerobotRawVisualType::RGB24;
	optional_ptr<atomic<bool>> cancelled;
};

struct LerobotEncodedVideoInfo {
	string codec;
	string pixel_format;
	double duration = 0;
	idx_t frame_count = 0;
};

struct LerobotVisualWriter {
	static bool Available();

	static LerobotEncodedVideoInfo EncodeVideo(FileSystem &fs, const string &path, const string &raw_frames_path,
	                                           idx_t frame_count, const LerobotVideoEncodeOptions &options);
	static void ConcatenateVideos(const vector<string> &input_paths, const string &list_path,
	                              const string &output_path);
	static string EncodeImage(const string &raw_frame, idx_t width, idx_t height, LerobotRawVisualType raw_type);

	static idx_t ExpectedFrameBytes(idx_t width, idx_t height, LerobotRawVisualType raw_type);
};

} // namespace duckdb
