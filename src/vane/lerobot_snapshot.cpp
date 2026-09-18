#ifdef LEROBOT_VANE_DISTRIBUTED
#include "vane/lerobot_snapshot.hpp"
#include "lerobot_query.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/unordered_set.hpp"
#include <algorithm>
#include <cmath>

namespace duckdb {

LerobotVaneDatasetSnapshot LerobotVaneDatasetSnapshot::Capture(ClientContext &context,
                                                               const LerobotDatasetMetadata &metadata) {
	LerobotVaneDatasetSnapshot result;
	result.root = metadata.GetRoot();
	result.fps = metadata.GetFPS();
	result.files = metadata.GetDataFiles();
	vector<Value> paths;
	for (const auto &file : metadata.GetEpisodeFiles()) {
		paths.emplace_back(file.path);
	}
	// Native metadata retains an episode-file fingerprint list only for remote
	// roots. Resolve local files once on the coordinator as well.
	if (paths.empty() && metadata.GetEpisodeCount()) {
		auto files = FileSystem::GetFileSystem(context).GlobFiles(result.root + "/meta/episodes/**/*.parquet",
		                                                          FileGlobOptions::DISALLOW_EMPTY);
		std::sort(files.begin(), files.end());
		for (const auto &file : files) {
			paths.emplace_back(file.path);
		}
	}
	if (!paths.empty()) {
		LerobotNestedQuery query(context, "SELECT CAST(episode_index AS BIGINT) FROM read_parquet(" +
		                                      Value::LIST(LogicalType::VARCHAR, std::move(paths)).ToSQLString() +
		                                      ") ORDER BY episode_index");
		if (query.GetResult().HasError()) {
			throw InvalidInputException("Failed to snapshot LeRobot episodes: %s", query.GetResult().GetError());
		}
		while (auto chunk = query.Fetch()) {
			for (idx_t row = 0; row < chunk->size(); row++) {
				auto episode = chunk->GetValue(0, row).GetValue<int64_t>();
				auto route = metadata.FindEpisodeRoute(episode);
				if (!route) {
					throw InvalidInputException("LeRobot metadata changed while creating the distributed snapshot");
				}
				result.episodes.push_back(episode);
				result.lengths.push_back(route->episode_length);
				result.file_ids.push_back(route->data_file_index);
			}
		}
	}
	if (result.episodes.size() != metadata.GetEpisodeCount()) {
		throw InvalidInputException("LeRobot episode snapshot changed: expected %llu episodes, read %llu",
		                            metadata.GetEpisodeCount(), result.episodes.size());
	}
	// Revalidation can legitimately replace a cache object. Compare its immutable
	// version fingerprints and routes, not process-local object identity.
	auto current = LerobotDatasetMetadata::Get(context, result.root, false);
	if (!(current->GetInfoFingerprint() == metadata.GetInfoFingerprint()) ||
	    current->GetEpisodeFiles() != metadata.GetEpisodeFiles() || current->GetDataFiles() != result.files ||
	    current->GetFPS() != result.fps || current->GetEpisodeCount() != result.episodes.size()) {
		throw InvalidInputException("LeRobot metadata changed while creating the distributed snapshot");
	}
	return result;
}

shared_ptr<LerobotDatasetMetadata> LerobotVaneDatasetSnapshot::Restore() const {
	if (fps <= 0 || episodes.size() != lengths.size() || episodes.size() != file_ids.size()) {
		throw SerializationException("Invalid LeRobot dataset snapshot");
	}
	vector<LerobotEpisodeRoute> restored;
	for (idx_t i = 0; i < episodes.size(); i++) {
		if (episodes[i] < 0 || lengths[i] <= 0 || file_ids[i] >= files.size() ||
		    (i && episodes[i] <= episodes[i - 1])) {
			throw SerializationException("Invalid LeRobot episode snapshot");
		}
		restored.emplace_back(episodes[i], lengths[i], file_ids[i]);
	}
	LerobotDatasetInfo info;
	info.fps = fps;
	info.total_episodes = episodes.size();
	return make_shared_ptr<LerobotDatasetMetadata>(root, std::move(info), std::move(restored), files,
	                                               LerobotDatasetMetadata::FileFingerprint(0, timestamp_t(0), ""));
}

void LerobotVaneDatasetSnapshot::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(1, "root", root);
	serializer.WriteProperty(2, "fps", fps);
	serializer.WriteProperty(3, "files", files);
	serializer.WriteProperty(4, "episodes", episodes);
	serializer.WriteProperty(5, "lengths", lengths);
	serializer.WriteProperty(6, "file_ids", file_ids);
}

LerobotVaneDatasetSnapshot LerobotVaneDatasetSnapshot::Deserialize(Deserializer &deserializer) {
	LerobotVaneDatasetSnapshot result;
	result.root = deserializer.ReadProperty<string>(1, "root");
	result.fps = deserializer.ReadProperty<int64_t>(2, "fps");
	result.files = deserializer.ReadProperty<vector<string>>(3, "files");
	result.episodes = deserializer.ReadProperty<vector<int64_t>>(4, "episodes");
	result.lengths = deserializer.ReadProperty<vector<int64_t>>(5, "lengths");
	result.file_ids = deserializer.ReadProperty<vector<idx_t>>(6, "file_ids");
	return result;
}

LerobotVaneVideoSnapshot LerobotVaneVideoSnapshot::Capture(const LerobotVideoMetadata &metadata,
                                                           const vector<LerobotVideoRoute> &routes) {
	LerobotVaneVideoSnapshot result;
	result.fps = metadata.GetFPS();
	for (const auto &route : routes) {
		const auto &feature = metadata.GetVideoFeatureMetadata(route);
		result.routes.push_back(Value::STRUCT({{"episode", Value::BIGINT(route.episode_index)},
		                                       {"length", Value::BIGINT(route.episode_length)},
		                                       {"key", Value(metadata.GetVideoKey(route))},
		                                       {"path", Value(metadata.GetVideoFile(route))},
		                                       {"chunk", Value::BIGINT(route.chunk_index)},
		                                       {"file", Value::BIGINT(route.file_index)},
		                                       {"from", Value::DOUBLE(route.from_timestamp)},
		                                       {"to", Value::DOUBLE(route.to_timestamp)},
		                                       {"depth", Value::BOOLEAN(feature.is_depth_map)},
		                                       {"min", Value::DOUBLE(feature.depth_min)},
		                                       {"max", Value::DOUBLE(feature.depth_max)},
		                                       {"shift", Value::DOUBLE(feature.shift)},
		                                       {"log", Value::BOOLEAN(feature.use_log)}}));
	}
	return result;
}

shared_ptr<LerobotVideoMetadata> LerobotVaneVideoSnapshot::Restore(vector<LerobotVideoRoute> &restored) const {
	if (fps <= 0) {
		throw SerializationException("Invalid LeRobot video snapshot FPS");
	}
	vector<string> keys, files;
	vector<LerobotVideoFeatureMetadata> features;
	restored.clear();
	for (const auto &value : routes) {
		const auto &fields = StructValue::GetChildren(value);
		if (fields.size() != 13 ||
		    std::any_of(fields.begin(), fields.end(), [](const Value &v) { return v.IsNull(); })) {
			throw SerializationException("Invalid LeRobot video route snapshot");
		}
		auto episode = fields[0].GetValue<int64_t>();
		auto length = fields[1].GetValue<int64_t>();
		auto key = fields[2].GetValue<string>();
		auto path = fields[3].GetValue<string>();
		auto from = fields[6].GetValue<double>();
		auto to = fields[7].GetValue<double>();
		if (episode < 0 || length <= 0 || key.empty() || path.empty() || !std::isfinite(from) || !std::isfinite(to) ||
		    from < 0 || to < from) {
			throw SerializationException("Invalid LeRobot video route values");
		}
		auto found = std::find(keys.begin(), keys.end(), key);
		idx_t key_id = found - keys.begin();
		if (found == keys.end()) {
			keys.push_back(key);
			LerobotVideoFeatureMetadata feature;
			feature.is_depth_map = fields[8].GetValue<bool>();
			feature.depth_min = fields[9].GetValue<double>();
			feature.depth_max = fields[10].GetValue<double>();
			feature.shift = fields[11].GetValue<double>();
			feature.use_log = fields[12].GetValue<bool>();
			features.push_back(feature);
		}
		auto file = std::find(files.begin(), files.end(), path);
		idx_t file_id = file - files.begin();
		if (file == files.end()) {
			files.push_back(path);
		}
		restored.emplace_back(episode, length, key_id, file_id, fields[4].GetValue<int64_t>(),
		                      fields[5].GetValue<int64_t>(), from, to);
	}
	// A camera may be absent from earlier episodes, so first-seen key order is
	// not lexical order. Native FindRoute/ResolveRoutes use binary search on keys.
	auto sorted_keys = keys;
	std::sort(sorted_keys.begin(), sorted_keys.end());
	vector<idx_t> key_indices(keys.size());
	vector<LerobotVideoFeatureMetadata> sorted_features(features.size());
	for (idx_t i = 0; i < keys.size(); i++) {
		auto index = std::lower_bound(sorted_keys.begin(), sorted_keys.end(), keys[i]) - sorted_keys.begin();
		key_indices[i] = index;
		sorted_features[index] = features[i];
	}
	keys = std::move(sorted_keys);
	features = std::move(sorted_features);
	for (auto &route : restored) {
		route.video_key_index = key_indices[route.video_key_index];
	}
	std::sort(restored.begin(), restored.end(), [](const LerobotVideoRoute &a, const LerobotVideoRoute &b) {
		return a.episode_index < b.episode_index ||
		       (a.episode_index == b.episode_index && a.video_key_index < b.video_key_index);
	});
	return make_shared_ptr<LerobotVideoMetadata>("", "", fps, std::move(keys), std::move(features), restored,
	                                             std::move(files),
	                                             LerobotDatasetMetadata::FileFingerprint(0, timestamp_t(0), ""));
}

void LerobotVaneVideoSnapshot::Serialize(Serializer &serializer) const {
	serializer.WriteProperty(1, "fps", fps);
	serializer.WriteProperty(2, "routes", routes);
}
LerobotVaneVideoSnapshot LerobotVaneVideoSnapshot::Deserialize(Deserializer &deserializer) {
	LerobotVaneVideoSnapshot result;
	result.fps = deserializer.ReadProperty<int64_t>(1, "fps");
	result.routes = deserializer.ReadProperty<vector<Value>>(2, "routes");
	return result;
}

void LerobotVaneSerializeOptions(Serializer &s, const LerobotVideoOptions &o) {
	s.WriteProperty(1, "tolerance", o.tolerance);
	s.WriteProperty(2, "cluster_gap", o.cluster_gap);
	s.WriteProperty(3, "width", o.width);
	s.WriteProperty(4, "height", o.height);
	s.WriteProperty(5, "output_batch_size", o.output_batch_size);
	s.WriteProperty(6, "target_buffer_size", o.target_buffer_size);
	s.WriteProperty(7, "max_cached_decoders", o.max_cached_decoders);
	s.WriteProperty(8, "decode_threads", o.decode_threads);
	s.WriteProperty(9, "producer_threads", o.producer_threads);
	s.WriteProperty(10, "max_pending_targets", o.max_pending_targets);
	s.WriteProperty(11, "max_output_bytes", o.max_output_bytes);
	s.WriteProperty(12, "codec_threads", o.codec_threads);
	s.WriteProperty(13, "depth_unit", static_cast<uint8_t>(o.depth_output_unit));
}
LerobotVideoOptions LerobotVaneDeserializeOptions(Deserializer &d) {
	LerobotVideoOptions o;
	o.tolerance = d.ReadProperty<double>(1, "tolerance");
	o.cluster_gap = d.ReadProperty<double>(2, "cluster_gap");
	o.width = d.ReadProperty<int32_t>(3, "width");
	o.height = d.ReadProperty<int32_t>(4, "height");
	o.output_batch_size = d.ReadProperty<idx_t>(5, "output_batch_size");
	o.target_buffer_size = d.ReadProperty<idx_t>(6, "target_buffer_size");
	o.max_cached_decoders = d.ReadProperty<idx_t>(7, "max_cached_decoders");
	o.decode_threads = d.ReadProperty<idx_t>(8, "decode_threads");
	o.producer_threads = d.ReadProperty<idx_t>(9, "producer_threads");
	o.max_pending_targets = d.ReadProperty<idx_t>(10, "max_pending_targets");
	o.max_output_bytes = d.ReadProperty<idx_t>(11, "max_output_bytes");
	o.codec_threads = d.ReadProperty<int32_t>(12, "codec_threads");
	auto unit = d.ReadProperty<uint8_t>(13, "depth_unit");
	if (!std::isfinite(o.tolerance) || o.tolerance < 0 || !std::isfinite(o.cluster_gap) || o.cluster_gap < 0 ||
	    !o.output_batch_size || !o.target_buffer_size || !o.max_cached_decoders || !o.decode_threads ||
	    !o.producer_threads || !o.max_pending_targets || !o.max_output_bytes || unit > 1) {
		throw SerializationException("Invalid LeRobot video options snapshot");
	}
	o.depth_output_unit = static_cast<LerobotDepthOutputUnit>(unit);
	return o;
}
void LerobotVaneSerializeDeltas(Serializer &s, const vector<LerobotTemporalDelta> &deltas) {
	vector<double> timestamps;
	vector<int64_t> offsets;
	for (const auto &delta : deltas) {
		timestamps.push_back(delta.timestamp);
		offsets.push_back(delta.frame_offset);
	}
	s.WriteProperty(1, "timestamps", timestamps);
	s.WriteProperty(2, "offsets", offsets);
}
vector<LerobotTemporalDelta> LerobotVaneDeserializeDeltas(Deserializer &d) {
	auto timestamps = d.ReadProperty<vector<double>>(1, "timestamps");
	auto offsets = d.ReadProperty<vector<int64_t>>(2, "offsets");
	if (timestamps.size() != offsets.size()) {
		throw SerializationException("Invalid LeRobot temporal delta snapshot");
	}
	vector<LerobotTemporalDelta> result;
	for (idx_t i = 0; i < timestamps.size(); i++) {
		if (!std::isfinite(timestamps[i])) {
			throw SerializationException("Invalid LeRobot temporal delta");
		}
		result.push_back({timestamps[i], offsets[i]});
	}
	return result;
}
} // namespace duckdb
#endif
