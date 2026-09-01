//===----------------------------------------------------------------------===//
//                         DuckDB
//
// storage/lerobot_metadata_cache.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/storage/object_cache.hpp"

namespace duckdb {

class ClientContext;

struct LerobotEpisodeRoute {
	LerobotEpisodeRoute(int64_t episode_index_p, idx_t data_file_index_p)
	    : episode_index(episode_index_p), data_file_index(data_file_index_p) {
	}

	int64_t episode_index;
	idx_t data_file_index;
};

struct LerobotVideoRoute {
	LerobotVideoRoute(int64_t episode_index_p, idx_t video_key_index_p, idx_t video_file_index_p, int64_t chunk_index_p,
	                  int64_t file_index_p, double from_timestamp_p, double to_timestamp_p)
	    : episode_index(episode_index_p), video_key_index(video_key_index_p), video_file_index(video_file_index_p),
	      chunk_index(chunk_index_p), file_index(file_index_p), from_timestamp(from_timestamp_p),
	      to_timestamp(to_timestamp_p) {
	}

	int64_t episode_index;
	idx_t video_key_index;
	idx_t video_file_index;
	int64_t chunk_index;
	int64_t file_index;
	double from_timestamp;
	double to_timestamp;
};

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

	LerobotDatasetMetadata(string root_p, string codebase_version_p, string data_path_template_p,
	                       string video_path_template_p, int64_t fps_p, vector<string> video_keys_p,
	                       vector<LerobotEpisodeRoute> routes_p, vector<string> data_files_p,
	                       FileFingerprint info_fingerprint_p);

	static shared_ptr<LerobotDatasetMetadata> Get(ClientContext &context, const string &root, bool refresh,
	                                              bool &cache_hit);
	static string ObjectType();

	string GetObjectType() override;
	optional_idx GetEstimatedCacheMemory() const override;

	vector<string> ResolveDataFiles(const vector<int64_t> &episode_indices) const;
	const string &GetSchemaDataFile() const;

	const string &GetRoot() const {
		return root;
	}
	const string &GetCodebaseVersion() const {
		return codebase_version;
	}
	const string &GetDataPathTemplate() const {
		return data_path_template;
	}
	const string &GetVideoPathTemplate() const {
		return video_path_template;
	}
	int64_t GetFPS() const {
		return fps;
	}
	const vector<string> &GetVideoKeys() const {
		return video_keys;
	}
	const FileFingerprint &GetInfoFingerprint() const {
		return info_fingerprint;
	}
	idx_t GetEpisodeCount() const {
		return routes.size();
	}
	idx_t GetDataFileCount() const {
		return data_files.size();
	}
	idx_t GetVideoKeyCount() const {
		return video_keys.size();
	}

private:
	static string CacheKey(const string &root);
	static FileFingerprint ReadInfoFingerprint(ClientContext &context, const string &root);
	static shared_ptr<LerobotDatasetMetadata> Load(ClientContext &context, const string &root,
	                                               const FileFingerprint &info_fingerprint);
	bool IsValid(ClientContext &context) const;

private:
	string root;
	string codebase_version;
	string data_path_template;
	string video_path_template;
	int64_t fps;
	vector<string> video_keys;
	vector<LerobotEpisodeRoute> routes;
	vector<string> data_files;
	FileFingerprint info_fingerprint;
};

//! Lazily loaded video control plane. Keeping this separate prevents ordinary
//! frame scans from materializing one route per episode and camera.
class LerobotVideoMetadata final : public ObjectCacheEntry {
public:
	LerobotVideoMetadata(string root_p, string video_path_template_p, int64_t fps_p, vector<string> video_keys_p,
	                     vector<LerobotVideoRoute> routes_p, vector<string> video_files_p,
	                     LerobotDatasetMetadata::FileFingerprint info_fingerprint_p);

	static shared_ptr<LerobotVideoMetadata> Get(ClientContext &context, const string &root, bool refresh,
	                                            bool &cache_hit);
	static string ObjectType();

	string GetObjectType() override;
	optional_idx GetEstimatedCacheMemory() const override;

	vector<LerobotVideoRoute> ResolveRoutes(const vector<int64_t> &episode_indices,
	                                        const vector<string> &requested_video_keys) const;
	const string &GetVideoKey(const LerobotVideoRoute &route) const {
		return video_keys[route.video_key_index];
	}
	const string &GetVideoFile(const LerobotVideoRoute &route) const {
		return video_files[route.video_file_index];
	}

	const string &GetRoot() const {
		return root;
	}
	const string &GetVideoPathTemplate() const {
		return video_path_template;
	}
	int64_t GetFPS() const {
		return fps;
	}
	const vector<string> &GetVideoKeys() const {
		return video_keys;
	}
	idx_t GetRouteCount() const {
		return routes.size();
	}
	idx_t GetVideoFileCount() const {
		return video_files.size();
	}

private:
	static string CacheKey(const string &root);
	static shared_ptr<LerobotVideoMetadata> Load(ClientContext &context, const LerobotDatasetMetadata &dataset);
	bool IsValid(const LerobotDatasetMetadata &dataset) const;

private:
	string root;
	string video_path_template;
	int64_t fps;
	vector<string> video_keys;
	vector<LerobotVideoRoute> routes;
	vector<string> video_files;
	LerobotDatasetMetadata::FileFingerprint info_fingerprint;
};

} // namespace duckdb
