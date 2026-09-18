#ifdef LEROBOT_VANE_DISTRIBUTED
// Included only by the Vane COPY translation unit.
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"

namespace duckdb {
namespace {
struct VaneFeatureSnapshot : LerobotFeature {
	VaneFeatureSnapshot() = default;
	explicit VaneFeatureSnapshot(const LerobotFeature &feature) : LerobotFeature(feature) {
	}
	void Serialize(Serializer &s) const {
		s.WriteProperty(1, "name", name);
		s.WriteProperty(2, "dtype", dtype);
		s.WriteProperty(3, "json", json);
		s.WriteProperty(4, "names_json", names_json);
		s.WriteProperty(5, "shape", shape);
		s.WriteProperty(6, "storage_type", storage_type);
		s.WriteProperty(7, "output_type", output_type);
		s.WriteProperty(8, "input_index", input_index);
		s.WriteProperty(9, "output_index", output_index);
		s.WriteProperty(10, "user_defined", user_defined);
		s.WriteProperty(11, "is_string", is_string);
		s.WriteProperty(12, "is_image", is_image);
		s.WriteProperty(13, "is_video", is_video);
		s.WriteProperty(14, "is_depth", is_depth);
	}
	static VaneFeatureSnapshot Deserialize(Deserializer &d) {
		VaneFeatureSnapshot r;
		r.name = d.ReadProperty<string>(1, "name");
		r.dtype = d.ReadProperty<string>(2, "dtype");
		r.json = d.ReadProperty<string>(3, "json");
		r.names_json = d.ReadProperty<string>(4, "names_json");
		r.shape = d.ReadProperty<vector<idx_t>>(5, "shape");
		r.storage_type = d.ReadProperty<LogicalType>(6, "storage_type");
		r.output_type = d.ReadProperty<LogicalType>(7, "output_type");
		r.input_index = d.ReadProperty<idx_t>(8, "input_index");
		r.output_index = d.ReadProperty<idx_t>(9, "output_index");
		r.user_defined = d.ReadProperty<bool>(10, "user_defined");
		r.is_string = d.ReadProperty<bool>(11, "is_string");
		r.is_image = d.ReadProperty<bool>(12, "is_image");
		r.is_video = d.ReadProperty<bool>(13, "is_video");
		r.is_depth = d.ReadProperty<bool>(14, "is_depth");
		return r;
	}
};
void VaneCopySerialize(Serializer &s, const FunctionData &data, const CopyFunction &) {
	const auto &b = data.Cast<LerobotCopyBindData>();
	s.WriteProperty(1, "input_names", b.input_names);
	s.WriteProperty(2, "input_types", b.input_types);
	s.WriteProperty(3, "data_names", b.data_names);
	s.WriteProperty(4, "data_types", b.data_types);
	s.WriteProperty(5, "episode_names", b.episode_names);
	s.WriteProperty(6, "episode_types", b.episode_types);
	s.WriteProperty(7, "episode_input_index", b.episode_input_index);
	s.WriteProperty(8, "task_input_index", b.task_input_index);
	s.WriteProperty(9, "fps", b.fps);
	s.WriteProperty(10, "chunks_size", b.chunks_size);
	s.WriteProperty(11, "data_file_size_mb", b.data_file_size_mb);
	s.WriteProperty(12, "video_file_size_mb", b.video_file_size_mb);
	s.WriteProperty(13, "metadata_buffer_size", b.metadata_buffer_size);
	s.WriteProperty(14, "max_visual_frame_bytes", b.max_visual_frame_bytes);
	s.WriteProperty(15, "video_workers", b.video_workers);
	s.WriteProperty(16, "encoder_threads", b.encoder_threads);
	s.WriteProperty(17, "robot_type", b.robot_type);
	s.WriteProperty(18, "has_robot_type", b.has_robot_type);
	s.WriteProperty(19, "features_json", b.features_json);
	vector<VaneFeatureSnapshot> features;
	for (const auto &feature : b.features) {
		features.emplace_back(feature);
	}
	s.WriteProperty(30, "features", features);
	s.WriteObject(31, "encoding", [&](Serializer &s) {
		s.WriteProperty(1, "rgb_codec", b.encoding.rgb_codec);
		s.WriteProperty(2, "rgb_crf", b.encoding.rgb_crf);
		s.WriteProperty(3, "rgb_gop", b.encoding.rgb_gop);
		s.WriteProperty(4, "depth_min", b.encoding.depth_min);
		s.WriteProperty(5, "depth_max", b.encoding.depth_max);
		s.WriteProperty(6, "depth_shift", b.encoding.depth_shift);
		s.WriteProperty(7, "depth_use_log", b.encoding.depth_use_log);
		s.WriteProperty(8, "depth_clip", b.encoding.depth_clip);
	});
}
unique_ptr<FunctionData> VaneCopyDeserialize(Deserializer &d, CopyFunction &) {
	auto b = make_uniq<LerobotCopyBindData>();
	b->input_names = d.ReadProperty<vector<string>>(1, "input_names");
	b->input_types = d.ReadProperty<vector<LogicalType>>(2, "input_types");
	b->data_names = d.ReadProperty<vector<string>>(3, "data_names");
	b->data_types = d.ReadProperty<vector<LogicalType>>(4, "data_types");
	b->episode_names = d.ReadProperty<vector<string>>(5, "episode_names");
	b->episode_types = d.ReadProperty<vector<LogicalType>>(6, "episode_types");
	b->episode_input_index = d.ReadProperty<idx_t>(7, "episode_input_index");
	b->task_input_index = d.ReadProperty<idx_t>(8, "task_input_index");
	b->fps = d.ReadProperty<idx_t>(9, "fps");
	b->chunks_size = d.ReadProperty<idx_t>(10, "chunks_size");
	b->data_file_size_mb = d.ReadProperty<double>(11, "data_file_size_mb");
	b->video_file_size_mb = d.ReadProperty<double>(12, "video_file_size_mb");
	b->metadata_buffer_size = d.ReadProperty<idx_t>(13, "metadata_buffer_size");
	b->max_visual_frame_bytes = d.ReadProperty<idx_t>(14, "max_visual_frame_bytes");
	b->video_workers = d.ReadProperty<idx_t>(15, "video_workers");
	b->encoder_threads = d.ReadProperty<idx_t>(16, "encoder_threads");
	b->robot_type = d.ReadProperty<string>(17, "robot_type");
	b->has_robot_type = d.ReadProperty<bool>(18, "has_robot_type");
	b->features_json = d.ReadProperty<string>(19, "features_json");
	auto features = d.ReadProperty<vector<VaneFeatureSnapshot>>(30, "features");
	for (const auto &feature : features) {
		b->features.push_back(feature);
	}
	d.ReadObject(31, "encoding", [&](Deserializer &d) {
		b->encoding.rgb_codec = d.ReadProperty<string>(1, "rgb_codec");
		b->encoding.rgb_crf = d.ReadProperty<int>(2, "rgb_crf");
		b->encoding.rgb_gop = d.ReadProperty<int>(3, "rgb_gop");
		b->encoding.depth_min = d.ReadProperty<double>(4, "depth_min");
		b->encoding.depth_max = d.ReadProperty<double>(5, "depth_max");
		b->encoding.depth_shift = d.ReadProperty<double>(6, "depth_shift");
		b->encoding.depth_use_log = d.ReadProperty<bool>(7, "depth_use_log");
		b->encoding.depth_clip = d.ReadProperty<bool>(8, "depth_clip");
	});
	b->parquet_function = GetParquetCopyFunction(d.Get<ClientContext &>());
	if (b->input_names.size() != b->input_types.size() || b->episode_input_index >= b->input_types.size() ||
	    b->task_input_index >= b->input_types.size() || !b->fps) {
		throw SerializationException("Invalid LeRobot COPY snapshot");
	}
	return std::move(b);
}
} // namespace
} // namespace duckdb
#endif
