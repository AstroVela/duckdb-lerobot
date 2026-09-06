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
class ClientContext;

enum class LerobotRawVisualType : uint8_t { RGB24, DEPTH_UINT16, DEPTH_FLOAT32 };

struct LerobotVideoEncodingConfig {
	//! Empty selects the build's default encoder; explicit SQL choices are strict.
	string rgb_codec;
	int rgb_crf = 30;
	int rgb_gop = 2;
	double depth_min = 0.01;
	double depth_max = 10;
	double depth_shift = 3.5;
	bool depth_use_log = true;
	bool depth_clip = true;

	bool operator==(const LerobotVideoEncodingConfig &other) const {
		return rgb_codec == other.rgb_codec && rgb_crf == other.rgb_crf && rgb_gop == other.rgb_gop &&
		       depth_min == other.depth_min && depth_max == other.depth_max && depth_shift == other.depth_shift &&
		       depth_use_log == other.depth_use_log && depth_clip == other.depth_clip;
	}
};

struct LerobotVideoEncodeOptions {
	idx_t width = 0;
	idx_t height = 0;
	idx_t fps = 0;
	optional_idx encoder_threads;
	LerobotRawVisualType raw_type = LerobotRawVisualType::RGB24;
	optional_ptr<atomic<bool>> cancelled;
	LerobotVideoEncodingConfig encoding;
};

struct LerobotEncodedVideoInfo {
	string encoder;
	string codec;
	string pixel_format;
	int gop = 0;
	double duration = 0;
	idx_t frame_count = 0;
};

//! One fixed-shape image feature in a COPY. Used by the serial COPY sink;
//! RGB reuses a PNG encoder and depth keeps the existing TIFF representation.
class LerobotImageWriter {
public:
	LerobotImageWriter(idx_t width, idx_t height, LerobotRawVisualType raw_type);
	~LerobotImageWriter();
	LerobotImageWriter(const LerobotImageWriter &) = delete;
	LerobotImageWriter &operator=(const LerobotImageWriter &) = delete;

	string Encode(const string &raw_frame);

private:
	struct Impl;
	idx_t width;
	idx_t height;
	LerobotRawVisualType raw_type;
	idx_t expected_size;
	unique_ptr<Impl> impl;
};

struct LerobotVisualWriter {
	static bool Available();

	static LerobotEncodedVideoInfo EncodeVideo(FileSystem &fs, const string &path, const string &raw_frames_path,
	                                           idx_t frame_count, const LerobotVideoEncodeOptions &options);
	static void ConcatenateVideos(ClientContext &context, FileSystem &fs, const vector<string> &input_paths,
	                              const string &output_path);
	static idx_t ExpectedFrameBytes(idx_t width, idx_t height, LerobotRawVisualType raw_type);
};

} // namespace duckdb
