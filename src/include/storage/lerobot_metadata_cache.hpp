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
	idx_t GetEpisodeCount() const {
		return routes.size();
	}
	idx_t GetDataFileCount() const {
		return data_files.size();
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
	vector<LerobotEpisodeRoute> routes;
	vector<string> data_files;
	FileFingerprint info_fingerprint;
};

} // namespace duckdb
