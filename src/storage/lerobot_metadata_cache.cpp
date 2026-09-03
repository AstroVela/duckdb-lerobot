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
#include <cmath>

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

struct ParsedLerobotInfo {
	string codebase_version;
	string data_path_template;
	string video_path_template;
	int64_t fps;
	vector<string> video_keys;
	vector<LerobotVideoFeatureMetadata> video_feature_metadata;
};

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

string FormatPathTemplate(const string &path_template, int64_t chunk_index, int64_t file_index, const string *video_key,
                          const char *path_name) {
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
				throw BinderException("Unclosed placeholder in LeRobot %s '%s'", path_name, path_template);
			}
			auto placeholder = path_template.substr(index + 1, close - index - 1);
			const auto colon = placeholder.find(':');
			const auto field_name = placeholder.substr(0, colon);
			const auto format_spec = colon == string::npos ? string() : placeholder.substr(colon + 1);
			if (field_name == "chunk_index") {
				result += FormatDecimal(chunk_index, format_spec, field_name);
			} else if (field_name == "file_index") {
				result += FormatDecimal(file_index, format_spec, field_name);
			} else if (field_name == "video_key" && video_key) {
				if (!format_spec.empty()) {
					throw BinderException("Unsupported LeRobot path format specifier '%s' for video_key", format_spec);
				}
				result += *video_key;
			} else {
				throw BinderException("Unsupported placeholder {%s} in LeRobot %s", placeholder, path_name);
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
			throw BinderException("Unmatched closing brace in LeRobot %s '%s'", path_name, path_template);
		}
		result.push_back(ch);
		index++;
	}
	return result;
}

string ResolveDatasetPath(const string &root, const string &path_template, int64_t chunk_index, int64_t file_index,
                          const string *video_key, const char *path_name) {
	auto relative_path = FormatPathTemplate(path_template, chunk_index, file_index, video_key, path_name);
	if (relative_path.empty()) {
		throw BinderException("LeRobot %s must not be empty", path_name);
	}
	if (relative_path[0] == '/' || StringUtil::Contains(relative_path, "://") ||
	    StringUtil::StartsWith(relative_path, "../") || StringUtil::Contains(relative_path, "/../") ||
	    StringUtil::EndsWith(relative_path, "/..")) {
		throw BinderException("LeRobot %s must stay relative to the dataset root: '%s'", path_name, relative_path);
	}
	return root + "/" + relative_path;
}

string ResolveDataPath(const string &root, const string &path_template, int64_t chunk_index, int64_t file_index) {
	return ResolveDatasetPath(root, path_template, chunk_index, file_index, nullptr, "data_path");
}

string ResolveVideoPath(const string &root, const string &path_template, const string &video_key, int64_t chunk_index,
                        int64_t file_index) {
	return ResolveDatasetPath(root, path_template, chunk_index, file_index, &video_key, "video_path");
}

string QuoteIdentifier(const string &identifier) {
	string result;
	result.reserve(identifier.size() + 2);
	result.push_back('"');
	for (const auto ch : identifier) {
		if (ch == '"') {
			result.push_back('"');
		}
		result.push_back(ch);
	}
	result.push_back('"');
	return result;
}

bool IsV3Version(const string &version) {
	return version == "v3" || version == "3" || StringUtil::StartsWith(version, "v3.") ||
	       StringUtil::StartsWith(version, "3.");
}

bool ValidFloat32DepthParameters(double depth_min, double depth_max, double shift, bool use_log) {
	const auto depth_min_float = static_cast<float>(depth_min);
	const auto depth_max_float = static_cast<float>(depth_max);
	const auto shift_float = static_cast<float>(shift);
	if (!std::isfinite(depth_min_float) || !std::isfinite(depth_max_float) || !std::isfinite(shift_float) ||
	    depth_min_float >= depth_max_float) {
		return false;
	}

	double scale;
	double offset;
	if (use_log) {
		const auto shifted_min = depth_min + shift;
		const auto shifted_max = depth_max + shift;
		if (!std::isfinite(shifted_min) || !std::isfinite(shifted_max) || shifted_min <= 0 ||
		    shifted_max <= shifted_min) {
			return false;
		}
		const auto log_min = std::log(shifted_min);
		const auto log_max = std::log(shifted_max);
		scale = (log_max - log_min) / 4095.0;
		offset = log_min;
	} else {
		scale = (depth_max - depth_min) / 4095.0;
		offset = depth_min;
	}
	const auto scale_float = static_cast<float>(scale);
	const auto offset_float = static_cast<float>(offset);
	return std::isfinite(scale) && std::isfinite(offset) && std::isfinite(scale_float) && scale_float > 0 &&
	       std::isfinite(offset_float);
}

ParsedLerobotInfo ReadLerobotInfo(Connection &connection, const string &info_path) {
	auto info_result = connection.Query(
	    "WITH info AS (SELECT row_number() OVER () AS record_index, json FROM read_json_objects(" +
	    MetadataQueryPath(info_path) +
	    ")) SELECT CAST(record_index AS BIGINT), json_extract_string(info.json, '$.codebase_version'), "
	    "json_extract_string(info.json, '$.data_path'), json_extract_string(info.json, '$.video_path'), "
	    "CAST(json_extract_string(info.json, '$.fps') AS BIGINT), features.key, "
	    "CAST(json_extract(features.value, '$.info.is_depth_map') AS BOOLEAN), "
	    "CAST(json_extract(features.value, '$.info.\"video.depth_min\"') AS DOUBLE), "
	    "CAST(json_extract(features.value, '$.info.\"video.depth_max\"') AS DOUBLE), "
	    "CAST(json_extract(features.value, '$.info.\"video.shift\"') AS DOUBLE), "
	    "CAST(json_extract(features.value, '$.info.\"video.use_log\"') AS BOOLEAN), "
	    "json_extract_string(features.value, '$.info.\"video.pix_fmt\"'), "
	    "CAST(json_extract(features.value, '$.shape[2]') AS BIGINT), "
	    "json_extract_string(features.value, '$.info.\"video.is_depth_map\"'), "
	    "json_extract_string(features.value, '$.video_info.\"video.is_depth_map\"') "
	    "FROM info LEFT JOIN LATERAL "
	    "json_each(json_extract(info.json, '$.features')) features ON "
	    "json_extract_string(features.value, '$.dtype') = 'video' ORDER BY record_index, features.key");
	if (info_result->HasError()) {
		ThrowQueryError("info.json", *info_result);
	}

	ParsedLerobotInfo info;
	unordered_set<string> video_keys_seen;
	bool found_record = false;
	while (true) {
		auto chunk = info_result->Fetch();
		if (!chunk) {
			break;
		}
		for (idx_t row = 0; row < chunk->size(); row++) {
			if (chunk->GetValue(0, row).IsNull()) {
				throw BinderException("LeRobot info.json produced a NULL metadata record index: '%s'", info_path);
			}
			const auto record_index = chunk->GetValue(0, row).GetValue<int64_t>();
			if (record_index != 1) {
				throw BinderException("LeRobot info.json must contain exactly one metadata record: '%s'", info_path);
			}
			if (!found_record) {
				if (chunk->GetValue(1, row).IsNull() || chunk->GetValue(2, row).IsNull() ||
				    chunk->GetValue(4, row).IsNull()) {
					throw BinderException(
					    "LeRobot info.json requires non-NULL codebase_version, data_path, and fps fields");
				}
				info.codebase_version = StringValue::Get(chunk->GetValue(1, row));
				info.data_path_template = StringValue::Get(chunk->GetValue(2, row));
				if (!chunk->GetValue(3, row).IsNull()) {
					info.video_path_template = StringValue::Get(chunk->GetValue(3, row));
				}
				info.fps = chunk->GetValue(4, row).GetValue<int64_t>();
				found_record = true;
			}
			if (!chunk->GetValue(5, row).IsNull()) {
				auto video_key = StringValue::Get(chunk->GetValue(5, row));
				if (video_key.empty()) {
					throw BinderException("LeRobot video feature keys must not be empty");
				}
				if (!video_keys_seen.insert(video_key).second) {
					throw BinderException("Duplicate LeRobot video feature key '%s' in info.json", video_key);
				}
				if (!chunk->GetValue(13, row).IsNull() || !chunk->GetValue(14, row).IsNull()) {
					throw BinderException(
					    "LeRobot video feature '%s' uses a legacy depth marker; use info.is_depth_map instead",
					    video_key);
				}
				LerobotVideoFeatureMetadata feature_metadata;
				const auto is_depth_map =
				    !chunk->GetValue(6, row).IsNull() && BooleanValue::Get(chunk->GetValue(6, row));
				if (is_depth_map) {
					for (idx_t column = 7; column <= 12; column++) {
						if (chunk->GetValue(column, row).IsNull()) {
							throw BinderException(
							    "LeRobot depth video feature '%s' requires video.depth_min, video.depth_max, "
							    "video.shift, video.use_log, video.pix_fmt, and a channel dimension",
							    video_key);
						}
					}
					const auto depth_min = chunk->GetValue(7, row).GetValue<double>();
					const auto depth_max = chunk->GetValue(8, row).GetValue<double>();
					const auto shift = chunk->GetValue(9, row).GetValue<double>();
					const auto use_log = BooleanValue::Get(chunk->GetValue(10, row));
					const auto pixel_format = StringValue::Get(chunk->GetValue(11, row));
					const auto channels = chunk->GetValue(12, row).GetValue<int64_t>();
					if (!std::isfinite(depth_min) || !std::isfinite(depth_max) || !std::isfinite(shift) ||
					    depth_min >= depth_max || !ValidFloat32DepthParameters(depth_min, depth_max, shift, use_log)) {
						throw BinderException("LeRobot depth video feature '%s' has invalid quantization parameters",
						                      video_key);
					}
					if (pixel_format != "gray12le") {
						throw BinderException("LeRobot depth video feature '%s' requires video.pix_fmt 'gray12le'",
						                      video_key);
					}
					if (channels != 1) {
						throw BinderException("LeRobot depth video feature '%s' requires exactly one channel",
						                      video_key);
					}
					feature_metadata = LerobotVideoFeatureMetadata(depth_min, depth_max, shift, use_log);
				} else {
					bool has_depth_only_metadata = false;
					for (idx_t column = 7; column <= 10; column++) {
						has_depth_only_metadata = has_depth_only_metadata || !chunk->GetValue(column, row).IsNull();
					}
					if (!chunk->GetValue(11, row).IsNull()) {
						has_depth_only_metadata =
						    has_depth_only_metadata || StringValue::Get(chunk->GetValue(11, row)) == "gray12le";
					}
					if (!chunk->GetValue(12, row).IsNull()) {
						has_depth_only_metadata =
						    has_depth_only_metadata || chunk->GetValue(12, row).GetValue<int64_t>() == 1;
					}
					if (has_depth_only_metadata) {
						throw BinderException(
						    "LeRobot video feature '%s' uses depth-only metadata but info.is_depth_map is not true",
						    video_key);
					}
				}
				info.video_keys.push_back(std::move(video_key));
				info.video_feature_metadata.push_back(feature_metadata);
			}
		}
	}
	if (!found_record) {
		throw BinderException("LeRobot info.json contains no metadata record: '%s'", info_path);
	}
	if (info.codebase_version.empty() || info.data_path_template.empty()) {
		throw BinderException("LeRobot info.json codebase_version and data_path fields must not be empty");
	}
	if (!IsV3Version(info.codebase_version)) {
		throw BinderException("LeRobot route pruning currently requires a v3 dataset, found codebase_version '%s'",
		                      info.codebase_version);
	}
	if (info.fps <= 0) {
		throw BinderException("LeRobot info.json fps must be positive");
	}
	if (!info.video_keys.empty() && info.video_path_template.empty()) {
		throw BinderException("LeRobot info.json requires video_path when video features are present");
	}
	return info;
}

} // namespace

LerobotDatasetMetadata::LerobotDatasetMetadata(string root_p, string codebase_version_p, string data_path_template_p,
                                               string video_path_template_p, int64_t fps_p, vector<string> video_keys_p,
                                               vector<LerobotVideoFeatureMetadata> video_feature_metadata_p,
                                               vector<LerobotEpisodeRoute> routes_p, vector<string> data_files_p,
                                               FileFingerprint info_fingerprint_p)
    : root(std::move(root_p)), codebase_version(std::move(codebase_version_p)),
      data_path_template(std::move(data_path_template_p)), video_path_template(std::move(video_path_template_p)),
      fps(fps_p), video_keys(std::move(video_keys_p)), video_feature_metadata(std::move(video_feature_metadata_p)),
      routes(std::move(routes_p)), data_files(std::move(data_files_p)),
      info_fingerprint(std::move(info_fingerprint_p)) {
	D_ASSERT(video_keys.size() == video_feature_metadata.size());
}

string LerobotDatasetMetadata::ObjectType() {
	return "lerobot_dataset_metadata";
}

string LerobotDatasetMetadata::GetObjectType() {
	return ObjectType();
}

optional_idx LerobotDatasetMetadata::GetEstimatedCacheMemory() const {
	idx_t memory = sizeof(*this);
	memory +=
	    root.capacity() + codebase_version.capacity() + data_path_template.capacity() + video_path_template.capacity();
	memory += video_keys.capacity() * sizeof(string);
	for (const auto &video_key : video_keys) {
		memory += video_key.capacity();
	}
	memory += video_feature_metadata.capacity() * sizeof(LerobotVideoFeatureMetadata);
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
	auto info = ReadLerobotInfo(connection, info_path);

	const auto episodes_path = root + LEROBOT_EPISODES_SUFFIX;
	auto episode_result = connection.Query(
	    "SELECT CAST(episode_index AS BIGINT), CAST(length AS BIGINT), CAST(\"data/chunk_index\" AS BIGINT), "
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
			for (idx_t column = 0; column < 4; column++) {
				if (chunk->GetValue(column, row).IsNull()) {
					throw BinderException("LeRobot episode routing columns must not contain NULL");
				}
			}
			const auto episode_index = chunk->GetValue(0, row).GetValue<int64_t>();
			const auto episode_length = chunk->GetValue(1, row).GetValue<int64_t>();
			const auto chunk_index = chunk->GetValue(2, row).GetValue<int64_t>();
			const auto file_index = chunk->GetValue(3, row).GetValue<int64_t>();
			if (episode_index < 0) {
				throw BinderException("LeRobot episode indices must be non-negative");
			}
			if (episode_length <= 0) {
				throw BinderException("LeRobot episode %d length must be positive", episode_index);
			}

			auto data_file = ResolveDataPath(root, info.data_path_template, chunk_index, file_index);
			auto entry = data_file_indexes.find(data_file);
			idx_t data_file_index;
			if (entry == data_file_indexes.end()) {
				data_file_index = data_files.size();
				data_file_indexes.emplace(data_file, data_file_index);
				data_files.push_back(std::move(data_file));
			} else {
				data_file_index = entry->second;
			}
			routes.emplace_back(episode_index, episode_length, data_file_index);
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

	return make_shared_ptr<LerobotDatasetMetadata>(
	    root, std::move(info.codebase_version), std::move(info.data_path_template), std::move(info.video_path_template),
	    info.fps, std::move(info.video_keys), std::move(info.video_feature_metadata), std::move(routes),
	    std::move(data_files), info_fingerprint);
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

const LerobotEpisodeRoute *LerobotDatasetMetadata::FindEpisodeRoute(int64_t episode_index) const {
	auto route = std::lower_bound(
	    routes.begin(), routes.end(), episode_index,
	    [](const LerobotEpisodeRoute &candidate, int64_t value) { return candidate.episode_index < value; });
	if (route == routes.end() || route->episode_index != episode_index) {
		return nullptr;
	}
	return &*route;
}

const string &LerobotDatasetMetadata::GetSchemaDataFile() const {
	static const string empty;
	return data_files.empty() ? empty : data_files[0];
}

LerobotVideoMetadata::LerobotVideoMetadata(string root_p, string video_path_template_p, int64_t fps_p,
                                           vector<string> video_keys_p,
                                           vector<LerobotVideoFeatureMetadata> video_feature_metadata_p,
                                           vector<LerobotVideoRoute> routes_p, vector<string> video_files_p,
                                           LerobotDatasetMetadata::FileFingerprint info_fingerprint_p)
    : root(std::move(root_p)), video_path_template(std::move(video_path_template_p)), fps(fps_p),
      video_keys(std::move(video_keys_p)), video_feature_metadata(std::move(video_feature_metadata_p)),
      routes(std::move(routes_p)), video_files(std::move(video_files_p)),
      info_fingerprint(std::move(info_fingerprint_p)) {
	D_ASSERT(video_keys.size() == video_feature_metadata.size());
}

string LerobotVideoMetadata::ObjectType() {
	return "lerobot_video_metadata";
}

string LerobotVideoMetadata::GetObjectType() {
	return ObjectType();
}

optional_idx LerobotVideoMetadata::GetEstimatedCacheMemory() const {
	idx_t memory = sizeof(*this) + root.capacity() + video_path_template.capacity();
	memory += video_keys.capacity() * sizeof(string);
	for (const auto &video_key : video_keys) {
		memory += video_key.capacity();
	}
	memory += video_feature_metadata.capacity() * sizeof(LerobotVideoFeatureMetadata);
	memory += routes.capacity() * sizeof(LerobotVideoRoute);
	memory += video_files.capacity() * sizeof(string);
	for (const auto &video_file : video_files) {
		memory += video_file.capacity();
	}
	memory += info_fingerprint.version_tag.capacity();
	return memory;
}

string LerobotVideoMetadata::CacheKey(const string &root) {
	return ObjectType() + ":" + root;
}

bool LerobotVideoMetadata::IsValid(const LerobotDatasetMetadata &dataset) const {
	return info_fingerprint == dataset.GetInfoFingerprint();
}

shared_ptr<LerobotVideoMetadata> LerobotVideoMetadata::Load(ClientContext &context,
                                                            const LerobotDatasetMetadata &dataset) {
	vector<LerobotVideoRoute> routes;
	vector<string> video_files;
	const auto &video_keys = dataset.GetVideoKeys();
	const auto &video_feature_metadata = dataset.GetVideoFeatureMetadata();
	if (video_keys.empty()) {
		return make_shared_ptr<LerobotVideoMetadata>(
		    dataset.GetRoot(), dataset.GetVideoPathTemplate(), dataset.GetFPS(), video_keys, video_feature_metadata,
		    std::move(routes), std::move(video_files), dataset.GetInfoFingerprint());
	}

	string query = "SELECT CAST(episode_index AS BIGINT), CAST(length AS BIGINT)";
	for (const auto &video_key : video_keys) {
		const auto prefix = "videos/" + video_key;
		query += ", CAST(" + QuoteIdentifier(prefix + "/chunk_index") + " AS BIGINT)";
		query += ", CAST(" + QuoteIdentifier(prefix + "/file_index") + " AS BIGINT)";
		query += ", CAST(" + QuoteIdentifier(prefix + "/from_timestamp") + " AS DOUBLE)";
		query += ", CAST(" + QuoteIdentifier(prefix + "/to_timestamp") + " AS DOUBLE)";
	}
	query += " FROM read_parquet(" + MetadataQueryPath(dataset.GetRoot() + LEROBOT_EPISODES_SUFFIX) + ")";

	Connection connection(*context.db);
	auto episode_result = connection.Query(query);
	if (episode_result->HasError()) {
		ThrowQueryError("episode video metadata", *episode_result);
	}

	unordered_map<string, idx_t> video_file_indexes;
	vector<idx_t> video_file_key_indices;
	while (true) {
		auto chunk = episode_result->Fetch();
		if (!chunk) {
			break;
		}
		for (idx_t row = 0; row < chunk->size(); row++) {
			if (chunk->GetValue(0, row).IsNull() || chunk->GetValue(1, row).IsNull()) {
				throw BinderException("LeRobot episode_index and length must not be NULL in video metadata");
			}
			const auto episode_index = chunk->GetValue(0, row).GetValue<int64_t>();
			const auto episode_length = chunk->GetValue(1, row).GetValue<int64_t>();
			if (episode_index < 0) {
				throw BinderException("LeRobot episode indices must be non-negative");
			}
			if (episode_length <= 0) {
				throw BinderException("LeRobot episode %d length must be positive", episode_index);
			}

			for (idx_t video_key_index = 0; video_key_index < video_keys.size(); video_key_index++) {
				const auto first_column = 2 + video_key_index * 4;
				idx_t null_count = 0;
				for (idx_t offset = 0; offset < 4; offset++) {
					if (chunk->GetValue(first_column + offset, row).IsNull()) {
						null_count++;
					}
				}
				if (null_count == 4) {
					continue;
				}
				if (null_count != 0) {
					throw BinderException("LeRobot video route for episode %d and key '%s' is partially NULL",
					                      episode_index, video_keys[video_key_index]);
				}

				const auto chunk_index = chunk->GetValue(first_column, row).GetValue<int64_t>();
				const auto file_index = chunk->GetValue(first_column + 1, row).GetValue<int64_t>();
				const auto from_timestamp = chunk->GetValue(first_column + 2, row).GetValue<double>();
				const auto to_timestamp = chunk->GetValue(first_column + 3, row).GetValue<double>();
				if (chunk_index < 0 || file_index < 0) {
					throw BinderException("LeRobot video chunk and file indices must be non-negative");
				}
				if (!std::isfinite(from_timestamp) || !std::isfinite(to_timestamp) || from_timestamp < 0 ||
				    to_timestamp < from_timestamp) {
					throw BinderException("Invalid LeRobot video timestamp range for episode %d and key '%s'",
					                      episode_index, video_keys[video_key_index]);
				}

				auto video_file = ResolveVideoPath(dataset.GetRoot(), dataset.GetVideoPathTemplate(),
				                                   video_keys[video_key_index], chunk_index, file_index);
				auto entry = video_file_indexes.find(video_file);
				idx_t video_file_index;
				if (entry == video_file_indexes.end()) {
					video_file_index = video_files.size();
					video_file_indexes.emplace(video_file, video_file_index);
					video_files.push_back(std::move(video_file));
					video_file_key_indices.push_back(video_key_index);
				} else {
					video_file_index = entry->second;
					D_ASSERT(video_file_index < video_file_key_indices.size());
					const auto previous_video_key_index = video_file_key_indices[video_file_index];
					if (previous_video_key_index != video_key_index) {
						throw BinderException("LeRobot video path '%s' is shared by feature keys '%s' and '%s'",
						                      video_file, video_keys[previous_video_key_index],
						                      video_keys[video_key_index]);
					}
				}
				routes.emplace_back(episode_index, episode_length, video_key_index, video_file_index, chunk_index,
				                    file_index, from_timestamp, to_timestamp);
			}
		}
	}

	std::sort(routes.begin(), routes.end(), [](const LerobotVideoRoute &left, const LerobotVideoRoute &right) {
		if (left.episode_index != right.episode_index) {
			return left.episode_index < right.episode_index;
		}
		return left.video_key_index < right.video_key_index;
	});
	for (idx_t index = 1; index < routes.size(); index++) {
		if (routes[index - 1].episode_index == routes[index].episode_index &&
		    routes[index - 1].video_key_index == routes[index].video_key_index) {
			throw BinderException("Duplicate LeRobot video route for episode %d and key '%s'",
			                      routes[index].episode_index, video_keys[routes[index].video_key_index]);
		}
	}

	return make_shared_ptr<LerobotVideoMetadata>(dataset.GetRoot(), dataset.GetVideoPathTemplate(), dataset.GetFPS(),
	                                             video_keys, video_feature_metadata, std::move(routes),
	                                             std::move(video_files), dataset.GetInfoFingerprint());
}

shared_ptr<LerobotVideoMetadata> LerobotVideoMetadata::Get(ClientContext &context, const string &root, bool refresh,
                                                           bool &cache_hit) {
	auto &cache = ObjectCache::GetObjectCache(context);
	const auto cache_key = CacheKey(root);
	bool dataset_cache_hit;
	auto dataset = LerobotDatasetMetadata::Get(context, root, refresh, dataset_cache_hit);
	cache_hit = false;
	if (!refresh) {
		auto cached = cache.Get<LerobotVideoMetadata>(cache_key);
		if (cached && cached->IsValid(*dataset)) {
			cache_hit = true;
			return cached;
		}
	}

	for (idx_t attempt = 0; attempt < 2; attempt++) {
		auto loaded = Load(context, *dataset);
		bool current_cache_hit;
		auto current = LerobotDatasetMetadata::Get(context, root, false, current_cache_hit);
		if (loaded->IsValid(*current)) {
			cache.Put(cache_key, loaded);
			return loaded;
		}
		dataset = std::move(current);
	}
	throw IOException("LeRobot info.json changed while video metadata was being loaded for '%s'", root);
}

vector<LerobotVideoRoute> LerobotVideoMetadata::ResolveRoutes(const vector<int64_t> &episode_indices,
                                                              const vector<string> &requested_video_keys) const {
	vector<idx_t> requested_key_indices;
	requested_key_indices.reserve(requested_video_keys.size());
	for (const auto &video_key : requested_video_keys) {
		auto entry = std::lower_bound(video_keys.begin(), video_keys.end(), video_key);
		if (entry == video_keys.end() || *entry != video_key) {
			throw BinderException("Unknown LeRobot video key '%s'", video_key);
		}
		requested_key_indices.push_back(static_cast<idx_t>(entry - video_keys.begin()));
	}
	std::sort(requested_key_indices.begin(), requested_key_indices.end());
	requested_key_indices.erase(std::unique(requested_key_indices.begin(), requested_key_indices.end()),
	                            requested_key_indices.end());

	vector<LerobotVideoRoute> result;
	for (const auto episode_index : episode_indices) {
		auto route = std::lower_bound(
		    routes.begin(), routes.end(), episode_index,
		    [](const LerobotVideoRoute &candidate, int64_t value) { return candidate.episode_index < value; });
		while (route != routes.end() && route->episode_index == episode_index) {
			if (std::binary_search(requested_key_indices.begin(), requested_key_indices.end(),
			                       route->video_key_index)) {
				result.push_back(*route);
			}
			route++;
		}
	}
	return result;
}

const LerobotVideoRoute *LerobotVideoMetadata::FindRoute(int64_t episode_index, const string &video_key) const {
	auto key_entry = std::lower_bound(video_keys.begin(), video_keys.end(), video_key);
	if (key_entry == video_keys.end() || *key_entry != video_key) {
		return nullptr;
	}
	const auto video_key_index = static_cast<idx_t>(key_entry - video_keys.begin());
	auto route = std::lower_bound(
	    routes.begin(), routes.end(), episode_index,
	    [](const LerobotVideoRoute &candidate, int64_t value) { return candidate.episode_index < value; });
	while (route != routes.end() && route->episode_index == episode_index) {
		if (route->video_key_index == video_key_index) {
			return &*route;
		}
		if (route->video_key_index > video_key_index) {
			break;
		}
		route++;
	}
	return nullptr;
}

} // namespace duckdb
