#include "storage/lerobot_metadata_cache.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"

#include <algorithm>

namespace duckdb {

namespace {

static const char *LEROBOT_INFO_SUFFIX = "/meta/info.json";
static const char *LEROBOT_EPISODES_SUFFIX = "/meta/episodes/**/*.parquet";

string MetadataQueryPath(const string &path) {
	return Value(path).ToSQLString();
}

void ThrowQueryError(const char *description, const QueryResult &result) {
	throw BinderException("Failed to read LeRobot %s: %s", description, result.GetError());
}

string FormatDecimal(int64_t value, const string &format_spec, const string &field_name) {
	if (value < 0) {
		throw BinderException("LeRobot %s must be non-negative", field_name);
	}

	auto result = std::to_string(value);
	if (format_spec.empty()) {
		return result;
	}
	if (format_spec.back() != 'd') {
		throw BinderException("Unsupported LeRobot path format specifier '%s' for %s", format_spec, field_name);
	}

	auto width_spec = format_spec.substr(0, format_spec.size() - 1);
	if (width_spec.empty()) {
		return result;
	}
	idx_t width = 0;
	for (const auto ch : width_spec) {
		if (ch < '0' || ch > '9') {
			throw BinderException("Unsupported LeRobot path format specifier '%s' for %s", format_spec, field_name);
		}
		if (width > 1000) {
			throw BinderException("LeRobot path format width is too large for %s", field_name);
		}
		width = width * 10 + static_cast<idx_t>(ch - '0');
	}
	if (width > 1000) {
		throw BinderException("LeRobot path format width is too large for %s", field_name);
	}
	if (width <= result.size()) {
		return result;
	}

	const auto fill = width_spec[0] == '0' ? '0' : ' ';
	return string(width - result.size(), fill) + result;
}

string FormatDataPath(const string &path_template, int64_t chunk_index, int64_t file_index) {
	string result;
	result.reserve(path_template.size());
	for (idx_t index = 0; index < path_template.size();) {
		const auto ch = path_template[index];
		if (ch == '{') {
			if (index + 1 < path_template.size() && path_template[index + 1] == '{') {
				result.push_back('{');
				index += 2;
				continue;
			}
			const auto close = path_template.find('}', index + 1);
			if (close == string::npos) {
				throw BinderException("Unclosed placeholder in LeRobot data_path '%s'", path_template);
			}
			auto placeholder = path_template.substr(index + 1, close - index - 1);
			const auto colon = placeholder.find(':');
			const auto field_name = placeholder.substr(0, colon);
			const auto format_spec = colon == string::npos ? string() : placeholder.substr(colon + 1);
			if (field_name == "chunk_index") {
				result += FormatDecimal(chunk_index, format_spec, field_name);
			} else if (field_name == "file_index") {
				result += FormatDecimal(file_index, format_spec, field_name);
			} else {
				throw BinderException("Unsupported placeholder {%s} in LeRobot data_path", placeholder);
			}
			index = close + 1;
			continue;
		}
		if (ch == '}') {
			if (index + 1 < path_template.size() && path_template[index + 1] == '}') {
				result.push_back('}');
				index += 2;
				continue;
			}
			throw BinderException("Unmatched closing brace in LeRobot data_path '%s'", path_template);
		}
		result.push_back(ch);
		index++;
	}
	return result;
}

string ResolveDataPath(const string &root, const string &path_template, int64_t chunk_index, int64_t file_index) {
	auto relative_path = FormatDataPath(path_template, chunk_index, file_index);
	if (relative_path.empty()) {
		throw BinderException("LeRobot data_path must not be empty");
	}
	if (relative_path[0] == '/' || StringUtil::Contains(relative_path, "://") ||
	    StringUtil::StartsWith(relative_path, "../") || StringUtil::Contains(relative_path, "/../") ||
	    StringUtil::EndsWith(relative_path, "/..")) {
		throw BinderException("LeRobot data_path must stay relative to the dataset root: '%s'", relative_path);
	}
	return root + "/" + relative_path;
}

bool IsV3Version(const string &version) {
	return version == "v3" || version == "3" || StringUtil::StartsWith(version, "v3.") ||
	       StringUtil::StartsWith(version, "3.");
}

} // namespace

LerobotDatasetMetadata::LerobotDatasetMetadata(string root_p, string codebase_version_p, string data_path_template_p,
                                               vector<LerobotEpisodeRoute> routes_p, vector<string> data_files_p,
                                               FileFingerprint info_fingerprint_p)
    : root(std::move(root_p)), codebase_version(std::move(codebase_version_p)),
      data_path_template(std::move(data_path_template_p)), routes(std::move(routes_p)),
      data_files(std::move(data_files_p)), info_fingerprint(std::move(info_fingerprint_p)) {
}

string LerobotDatasetMetadata::ObjectType() {
	return "lerobot_dataset_metadata";
}

string LerobotDatasetMetadata::GetObjectType() {
	return ObjectType();
}

optional_idx LerobotDatasetMetadata::GetEstimatedCacheMemory() const {
	idx_t memory = sizeof(*this);
	memory += root.capacity() + codebase_version.capacity() + data_path_template.capacity();
	memory += routes.capacity() * sizeof(LerobotEpisodeRoute);
	memory += data_files.capacity() * sizeof(string);
	for (const auto &path : data_files) {
		memory += path.capacity();
	}
	memory += info_fingerprint.version_tag.capacity();
	return memory;
}

string LerobotDatasetMetadata::CacheKey(const string &root) {
	return ObjectType() + ":" + root;
}

LerobotDatasetMetadata::FileFingerprint LerobotDatasetMetadata::ReadInfoFingerprint(ClientContext &context,
                                                                                    const string &root) {
	auto &file_system = FileSystem::GetFileSystem(context);
	auto handle = file_system.OpenFile(root + LEROBOT_INFO_SUFFIX, FileFlags::FILE_FLAGS_READ);
	return FileFingerprint(file_system.GetFileSize(*handle), file_system.GetLastModifiedTime(*handle),
	                       file_system.GetVersionTag(*handle));
}

bool LerobotDatasetMetadata::IsValid(ClientContext &context) const {
	return info_fingerprint == ReadInfoFingerprint(context, root);
}

shared_ptr<LerobotDatasetMetadata> LerobotDatasetMetadata::Load(ClientContext &context, const string &root,
                                                                const FileFingerprint &info_fingerprint) {
	Connection connection(*context.db);
	const auto info_path = root + LEROBOT_INFO_SUFFIX;
	auto info_result = connection.Query("SELECT CAST(codebase_version AS VARCHAR), CAST(data_path AS VARCHAR) "
	                                    "FROM read_json_auto(" +
	                                    MetadataQueryPath(info_path) + ") LIMIT 2");
	if (info_result->HasError()) {
		ThrowQueryError("info.json", *info_result);
	}
	auto info_chunk = info_result->Fetch();
	if (!info_chunk || info_chunk->size() == 0) {
		throw BinderException("LeRobot info.json contains no metadata record: '%s'", info_path);
	}
	if (info_chunk->GetValue(0, 0).IsNull() || info_chunk->GetValue(1, 0).IsNull()) {
		throw BinderException("LeRobot info.json requires non-NULL codebase_version and data_path fields");
	}
	auto codebase_version = StringValue::Get(info_chunk->GetValue(0, 0));
	auto data_path_template = StringValue::Get(info_chunk->GetValue(1, 0));
	if (info_chunk->size() > 1) {
		throw BinderException("LeRobot info.json must contain exactly one metadata record: '%s'", info_path);
	}
	if (!IsV3Version(codebase_version)) {
		throw BinderException("LeRobot route pruning currently requires a v3 dataset, found codebase_version '%s'",
		                      codebase_version);
	}

	const auto episodes_path = root + LEROBOT_EPISODES_SUFFIX;
	auto episode_result =
	    connection.Query("SELECT CAST(episode_index AS BIGINT), CAST(\"data/chunk_index\" AS BIGINT), "
	                     "CAST(\"data/file_index\" AS BIGINT) FROM read_parquet(" +
	                     MetadataQueryPath(episodes_path) + ")");
	if (episode_result->HasError()) {
		ThrowQueryError("episode metadata", *episode_result);
	}

	vector<LerobotEpisodeRoute> routes;
	vector<string> data_files;
	unordered_map<string, idx_t> data_file_indexes;
	while (true) {
		auto chunk = episode_result->Fetch();
		if (!chunk) {
			break;
		}
		for (idx_t row = 0; row < chunk->size(); row++) {
			for (idx_t column = 0; column < 3; column++) {
				if (chunk->GetValue(column, row).IsNull()) {
					throw BinderException("LeRobot episode routing columns must not contain NULL");
				}
			}
			const auto episode_index = chunk->GetValue(0, row).GetValue<int64_t>();
			const auto chunk_index = chunk->GetValue(1, row).GetValue<int64_t>();
			const auto file_index = chunk->GetValue(2, row).GetValue<int64_t>();
			if (episode_index < 0) {
				throw BinderException("LeRobot episode indices must be non-negative");
			}

			auto data_file = ResolveDataPath(root, data_path_template, chunk_index, file_index);
			auto entry = data_file_indexes.find(data_file);
			idx_t data_file_index;
			if (entry == data_file_indexes.end()) {
				data_file_index = data_files.size();
				data_file_indexes.emplace(data_file, data_file_index);
				data_files.push_back(std::move(data_file));
			} else {
				data_file_index = entry->second;
			}
			routes.emplace_back(episode_index, data_file_index);
		}
	}

	std::sort(routes.begin(), routes.end(), [](const LerobotEpisodeRoute &left, const LerobotEpisodeRoute &right) {
		return left.episode_index < right.episode_index;
	});
	for (idx_t index = 1; index < routes.size(); index++) {
		if (routes[index - 1].episode_index == routes[index].episode_index) {
			throw BinderException("Duplicate LeRobot episode_index %d in episode metadata",
			                      routes[index].episode_index);
		}
	}

	return make_shared_ptr<LerobotDatasetMetadata>(root, std::move(codebase_version), std::move(data_path_template),
	                                               std::move(routes), std::move(data_files), info_fingerprint);
}

shared_ptr<LerobotDatasetMetadata> LerobotDatasetMetadata::Get(ClientContext &context, const string &root, bool refresh,
                                                               bool &cache_hit) {
	auto &cache = ObjectCache::GetObjectCache(context);
	const auto cache_key = CacheKey(root);
	cache_hit = false;
	if (!refresh) {
		auto cached = cache.Get<LerobotDatasetMetadata>(cache_key);
		if (cached && cached->IsValid(context)) {
			cache_hit = true;
			return cached;
		}
	}
	// Treat info.json as the dataset commit marker. A changing marker retries
	// once so routes and path templates cannot be cached across an update.
	for (idx_t attempt = 0; attempt < 2; attempt++) {
		auto before = ReadInfoFingerprint(context, root);
		auto loaded = Load(context, root, before);
		auto after = ReadInfoFingerprint(context, root);
		if (before == after) {
			cache.Put(cache_key, loaded);
			return loaded;
		}
	}
	throw IOException("LeRobot info.json changed while metadata was being loaded for '%s'", root);
}

vector<string> LerobotDatasetMetadata::ResolveDataFiles(const vector<int64_t> &episode_indices) const {
	vector<string> result;
	unordered_set<string> seen;
	for (const auto episode_index : episode_indices) {
		auto route = std::lower_bound(
		    routes.begin(), routes.end(), episode_index,
		    [](const LerobotEpisodeRoute &candidate, int64_t value) { return candidate.episode_index < value; });
		if (route == routes.end() || route->episode_index != episode_index) {
			continue;
		}
		const auto &path = data_files[route->data_file_index];
		if (seen.insert(path).second) {
			result.push_back(path);
		}
	}
	std::sort(result.begin(), result.end());
	return result;
}

const string &LerobotDatasetMetadata::GetSchemaDataFile() const {
	static const string empty;
	return data_files.empty() ? empty : data_files[0];
}

} // namespace duckdb
