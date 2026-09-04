//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_codec_executor.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "function/lerobot_video_writer.hpp"

#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

class ClientContext;
class FileSystem;

struct LerobotCodecJob {
	idx_t feature_index = 0;
	string output_path;
	string raw_frames_path;
	idx_t frame_count = 0;
	LerobotVideoEncodeOptions options;
};

struct LerobotCodecResult {
	idx_t feature_index = 0;
	string output_path;
	LerobotEncodedVideoInfo encoded;
};

//! A database-instance executor for long-running FFmpeg work. It deliberately
//! does not use DuckDB's pipeline workers: COPY waits for a batch while these
//! dedicated workers encode it, then publishes the results in caller order.
class LerobotCodecExecutor : public ObjectCacheEntry {
public:
	LerobotCodecExecutor();
	~LerobotCodecExecutor() override;

	static string ObjectType();
	string GetObjectType() override;
	optional_idx GetEstimatedCacheMemory() const override;

	static shared_ptr<LerobotCodecExecutor> Get(ClientContext &context);

	vector<LerobotCodecResult> Execute(ClientContext &context, FileSystem &fs, vector<LerobotCodecJob> jobs,
	                                   idx_t max_workers, idx_t codec_thread_budget);

private:
	struct Impl;
	unique_ptr<Impl> impl;
};

} // namespace duckdb
