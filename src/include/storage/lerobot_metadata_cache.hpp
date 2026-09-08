//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/lerobot_metadata_cache.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

class ClientContext;

struct LerobotEpisodeRoute {
	LerobotEpisodeRoute(int64_t episode_index_p, int64_t episode_length_p, idx_t data_file_index_p)
	    : episode_index(episode_index_p), episode_length(episode_length_p), data_file_index(data_file_index_p) {
	}

	int64_t episode_index;
	int64_t episode_length;
	idx_t data_file_index;
};

struct LerobotVideoRoute {
	LerobotVideoRoute(int64_t episode_index_p, int64_t episode_length_p, idx_t video_key_index_p,
	                  idx_t video_file_index_p, int64_t chunk_index_p, int64_t file_index_p, double from_timestamp_p,
	                  double to_timestamp_p)
	    : episode_index(episode_index_p), video_key_index(video_key_index_p), video_file_index(video_file_index_p),
	      episode_length(episode_length_p), chunk_index(chunk_index_p), file_index(file_index_p),
	      from_timestamp(from_timestamp_p), to_timestamp(to_timestamp_p) {
	}

	int64_t episode_index;
	idx_t video_key_index;
	idx_t video_file_index;
	int64_t episode_length;
	int64_t chunk_index;
	int64_t file_index;
	double from_timestamp;
	double to_timestamp;
};

struct LerobotVideoFeatureMetadata {
	LerobotVideoFeatureMetadata() : is_depth_map(false), depth_min(0), depth_max(0), shift(0), use_log(false) {
	}

	LerobotVideoFeatureMetadata(double depth_min_p, double depth_max_p, double shift_p, bool use_log_p)
	    : is_depth_map(true), depth_min(depth_min_p), depth_max(depth_max_p), shift(shift_p), use_log(use_log_p) {
	}

	bool is_depth_map;
	double depth_min;
	double depth_max;
	double shift;
	bool use_log;
};

struct LerobotScanSchema {
	vector<string> names;
	vector<LogicalType> types;
};

struct LerobotDatasetInfo {
	LerobotDatasetInfo() : fps(0), total_episodes(0), total_frames(0), total_tasks(0) {
	}

	string codebase_version;
	string data_path_template;
	string video_path_template;
	int64_t fps;
	int64_t total_episodes;
	int64_t total_frames;
	int64_t total_tasks;
	vector<string> video_keys;
	vector<LerobotVideoFeatureMetadata> video_feature_metadata;
	LerobotScanSchema frame_schema;
	LerobotScanSchema episode_schema;
};

LerobotDatasetInfo ReadLerobotDatasetInfo(ClientContext &context, const string &root);

//! Immutable, dataset-level metadata used to route an episode to a small set
//! of Parquet shards. Parquet footers and row-group statistics remain owned by
//! DuckDB's native Parquet caches.
class LerobotDatasetMetadata final : public ObjectCacheEntry {
public:
	struct FileFingerprint {
		FileFingerprint(int64_t size_p, timestamp_t last_modified_p, string version_tag_p)
		    : size(size_p), last_modified(last_modified_p), version_tag(std::move(version_tag_p)) {
		}

		bool operator==(const FileFingerprint &other) const {
			return size == other.size && last_modified == other.last_modified && version_tag == other.version_tag;
		}

		int64_t size;
		timestamp_t last_modified;
		string version_tag;
	};

	struct MetadataFile {
		MetadataFile(string path_p, FileFingerprint fingerprint_p)
		    : path(std::move(path_p)), fingerprint(std::move(fingerprint_p)) {
		}
		bool operator==(const MetadataFile &other) const {
			return path == other.path && fingerprint == other.fingerprint;
		}
		string path;
		FileFingerprint fingerprint;
	};

	LerobotDatasetMetadata(string root_p, LerobotDatasetInfo info_p, vector<LerobotEpisodeRoute> routes_p,
	                       vector<string> data_files_p, FileFingerprint info_fingerprint_p,
	                       vector<MetadataFile> episode_files_p = {});

	static shared_ptr<LerobotDatasetMetadata> Get(ClientContext &context, const string &root, bool refresh);
	//! Return an existing cache entry without reading dataset storage or creating
	//! a new entry.
	static shared_ptr<LerobotDatasetMetadata> Peek(ClientContext &context, const string &root);
	//! Remove both data and video routing entries for this dataset root.
	static void Invalidate(ClientContext &context, const string &root);
	static string ObjectType();

	string GetObjectType() override;
	optional_idx GetEstimatedCacheMemory() const override;

	vector<string> ResolveDataFiles(const vector<int64_t> &episode_indices) const;
	const LerobotEpisodeRoute *FindEpisodeRoute(int64_t episode_index) const;
	const vector<string> &GetDataFiles() const {
		return data_files;
	}

	const string &GetRoot() const {
		return root;
	}
	const string &GetVideoPathTemplate() const {
		return info.video_path_template;
	}
	int64_t GetFPS() const {
		return info.fps;
	}
	const vector<string> &GetVideoKeys() const {
		return info.video_keys;
	}
	const vector<LerobotVideoFeatureMetadata> &GetVideoFeatureMetadata() const {
		return info.video_feature_metadata;
	}
	const LerobotScanSchema &GetFrameSchema() const {
		return info.frame_schema;
	}
	const FileFingerprint &GetInfoFingerprint() const {
		return info_fingerprint;
	}
	const vector<MetadataFile> &GetEpisodeFiles() const {
		return episode_files;
	}
	idx_t GetEpisodeCount() const {
		return static_cast<idx_t>(info.total_episodes);
	}

private:
	static string CacheKey(ClientContext &context, const string &root);
	static FileFingerprint ReadInfoFingerprint(ClientContext &context, const string &root);
	static vector<MetadataFile> ReadEpisodeFiles(ClientContext &context, const string &root);
	static shared_ptr<LerobotDatasetMetadata> Load(ClientContext &context, const string &root,
	                                               const FileFingerprint &info_fingerprint);
	bool IsValid(ClientContext &context) const;
	bool EpisodeFilesAreValid(ClientContext &context) const;

private:
	string root;
	LerobotDatasetInfo info;
	vector<LerobotEpisodeRoute> routes;
	vector<string> data_files;
	FileFingerprint info_fingerprint;
	vector<MetadataFile> episode_files;
};

//! Lazily loaded video control plane. Keeping this separate prevents ordinary
//! frame scans from materializing one route per episode and camera.
class LerobotVideoMetadata final : public ObjectCacheEntry {
public:
	LerobotVideoMetadata(string root_p, string video_path_template_p, int64_t fps_p, vector<string> video_keys_p,
	                     vector<LerobotVideoFeatureMetadata> video_feature_metadata_p,
	                     vector<LerobotVideoRoute> routes_p, vector<string> video_files_p,
	                     LerobotDatasetMetadata::FileFingerprint info_fingerprint_p,
	                     vector<LerobotDatasetMetadata::MetadataFile> episode_files_p = {});

	static shared_ptr<LerobotVideoMetadata> Get(ClientContext &context, const string &root, bool refresh);
	//! Return an existing cache entry without reading dataset storage or creating
	//! a new entry.
	static shared_ptr<LerobotVideoMetadata> Peek(ClientContext &context, const string &root);
	static void Invalidate(ClientContext &context, const string &root);
	static string ObjectType();

	string GetObjectType() override;
	optional_idx GetEstimatedCacheMemory() const override;

	vector<LerobotVideoRoute> ResolveRoutes(const vector<int64_t> &episode_indices,
	                                        const vector<string> &requested_video_keys) const;
	//! Resolve one episode/camera route without copying the dataset-wide route table.
	//! The returned pointer remains valid for the lifetime of this immutable cache entry.
	const LerobotVideoRoute *FindRoute(int64_t episode_index, const string &video_key) const;
	const string &GetVideoKey(const LerobotVideoRoute &route) const {
		return video_keys[route.video_key_index];
	}
	const string &GetVideoFile(const LerobotVideoRoute &route) const {
		return video_files[route.video_file_index];
	}
	const LerobotVideoFeatureMetadata &GetVideoFeatureMetadata(const LerobotVideoRoute &route) const {
		return video_feature_metadata[route.video_key_index];
	}

	int64_t GetFPS() const {
		return fps;
	}
	const vector<string> &GetVideoKeys() const {
		return video_keys;
	}

private:
	static string CacheKey(ClientContext &context, const string &root);
	static shared_ptr<LerobotVideoMetadata> Load(ClientContext &context, const LerobotDatasetMetadata &dataset);
	bool IsValid(const LerobotDatasetMetadata &dataset) const;

private:
	string root;
	string video_path_template;
	int64_t fps;
	vector<string> video_keys;
	vector<LerobotVideoFeatureMetadata> video_feature_metadata;
	vector<LerobotVideoRoute> routes;
	vector<string> video_files;
	LerobotDatasetMetadata::FileFingerprint info_fingerprint;
	vector<LerobotDatasetMetadata::MetadataFile> episode_files;
};

} // namespace duckdb
