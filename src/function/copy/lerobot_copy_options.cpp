#include "function/lerobot_copy_options.hpp"
#include "lerobot_depth.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <cmath>

namespace duckdb {

namespace {

static const idx_t LEROBOT_DEFAULT_CHUNK_SIZE = 1000;
static const double LEROBOT_DEFAULT_DATA_FILE_SIZE_MB = 100;
static const double LEROBOT_DEFAULT_VIDEO_FILE_SIZE_MB = 200;
static const idx_t LEROBOT_DEFAULT_METADATA_BUFFER_SIZE = 10;
static const idx_t LEROBOT_DEFAULT_MAX_VISUAL_FRAME_BYTES = 64ULL * 1024ULL * 1024ULL;
static const idx_t LEROBOT_DEFAULT_VIDEO_WORKERS = 4;
static const idx_t LEROBOT_DEFAULT_ENCODER_THREADS = 4;

const Value &GetSingleOption(const CopyFunctionBindInput &input, const char *name, bool required) {
	auto entry = input.info.options.find(name);
	if (entry == input.info.options.end()) {
		if (required) {
			throw BinderException("FORMAT lerobot requires the %s option", StringUtil::Upper(name));
		}
		static Value null_value;
		return null_value;
	}
	if (entry->second.size() != 1 || entry->second[0].IsNull()) {
		throw BinderException("FORMAT lerobot option %s requires exactly one non-NULL value", StringUtil::Upper(name));
	}
	return entry->second[0];
}

template <class T>
T GetNumericOption(const CopyFunctionBindInput &input, const char *name, T default_value) {
	const auto &value = GetSingleOption(input, name, false);
	if (value.IsNull()) {
		return default_value;
	}
	return value.DefaultCastAs(LogicalType::DOUBLE).GetValue<T>();
}

} // namespace

LerobotCopyConfig ParseLerobotCopyRequiredConfig(const CopyFunctionBindInput &input) {
	LerobotCopyConfig result;
	result.chunks_size = 0;
	result.data_file_size_mb = 0;
	result.video_file_size_mb = 0;
	result.metadata_buffer_size = 0;
	result.max_visual_frame_bytes = 0;
	result.video_workers = 0;
	result.encoder_threads = 0;
	result.has_robot_type = false;

	const auto &fps_value = GetSingleOption(input, "fps", true);
	auto fps = fps_value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
	if (fps <= 0) {
		throw BinderException("LeRobot FPS must be positive");
	}
	if (static_cast<uint64_t>(fps) > NumericLimits<idx_t>::Maximum()) {
		throw BinderException("LeRobot FPS is too large");
	}
	result.fps = static_cast<idx_t>(fps);

	const auto &features_value = GetSingleOption(input, "features", true);
	result.features_json = StringValue::Get(features_value.DefaultCastAs(LogicalType::VARCHAR));
	return result;
}

void ParseLerobotCopyOptionalConfig(ClientContext &context, const CopyFunctionBindInput &input,
                                    LerobotCopyConfig &result) {
	const auto host_threads = MaxValue<idx_t>(1, context.db->NumberOfThreads());
	result.video_workers = MinValue(LEROBOT_DEFAULT_VIDEO_WORKERS, host_threads);
	result.encoder_threads = MinValue(LEROBOT_DEFAULT_ENCODER_THREADS, host_threads);

	auto &encoding = result.encoding;
	auto codec = GetSingleOption(input, "rgb_codec", false);
	if (!codec.IsNull()) {
		encoding.rgb_codec = StringValue::Get(codec.DefaultCastAs(LogicalType::VARCHAR));
		if (encoding.rgb_codec != "libsvtav1" && encoding.rgb_codec != "libaom-av1") {
			throw BinderException("LeRobot RGB_CODEC must be 'libsvtav1' or 'libaom-av1'");
		}
	}
	auto crf = GetNumericOption<double>(input, "rgb_crf", 30);
	auto gop = GetNumericOption<double>(input, "rgb_gop", 2);
	if (!std::isfinite(crf) || crf < 0 || crf > 63 || std::floor(crf) != crf) {
		throw BinderException("LeRobot RGB_CRF must be an integer between 0 and 63");
	}
	if (!std::isfinite(gop) || gop < 1 || gop > NumericLimits<int>::Maximum() || std::floor(gop) != gop) {
		throw BinderException("LeRobot RGB_GOP must be a positive 32-bit integer");
	}
	encoding.rgb_crf = static_cast<int>(crf);
	encoding.rgb_gop = static_cast<int>(gop);
	encoding.depth_min = GetNumericOption<double>(input, "depth_min", 0.01);
	encoding.depth_max = GetNumericOption<double>(input, "depth_max", 10);
	encoding.depth_shift = GetNumericOption<double>(input, "depth_shift", 3.5);
	auto use_log = GetSingleOption(input, "depth_use_log", false);
	auto clip = GetSingleOption(input, "depth_clip", false);
	if (!use_log.IsNull()) {
		encoding.depth_use_log = use_log.DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>();
	}
	if (!clip.IsNull()) {
		encoding.depth_clip = clip.DefaultCastAs(LogicalType::BOOLEAN).GetValue<bool>();
	}
	if (!std::isfinite(encoding.depth_min) || !std::isfinite(encoding.depth_max) ||
	    !std::isfinite(encoding.depth_shift) || encoding.depth_min < 0 || encoding.depth_max <= encoding.depth_min ||
	    (encoding.depth_use_log && encoding.depth_min + encoding.depth_shift <= 0)) {
		throw BinderException("LeRobot depth parameters require finite 0 <= DEPTH_MIN < DEPTH_MAX and, for log "
		                      "quantization, DEPTH_MIN + DEPTH_SHIFT > 0");
	}
	if (!LerobotValidFloat32DepthParameters(encoding.depth_min, encoding.depth_max, encoding.depth_shift,
	                                        encoding.depth_use_log)) {
		throw BinderException("LeRobot depth parameters are not representable for float32 dequantization");
	}
	// Both accepted input units use float32 arithmetic, matching LeRobot.
	for (const auto scale : {1.0, 1000.0}) {
		const auto low = static_cast<float>(encoding.depth_min * scale);
		const auto high = static_cast<float>(encoding.depth_max * scale);
		const auto shift = static_cast<float>(encoding.depth_shift * scale);
		if (!std::isfinite(low) || !std::isfinite(high) || !std::isfinite(shift) || !(high > low) ||
		    (encoding.depth_use_log &&
		     (!(low + shift > 0) || !std::isfinite(high + shift) || !(high + shift > low + shift)))) {
			throw BinderException("LeRobot depth parameters are not representable in float32 metres and millimetres");
		}
	}
	auto chunks_size = GetNumericOption<double>(input, "chunks_size", LEROBOT_DEFAULT_CHUNK_SIZE);
	auto metadata_buffer_size =
	    GetNumericOption<double>(input, "metadata_buffer_size", LEROBOT_DEFAULT_METADATA_BUFFER_SIZE);
	result.data_file_size_mb =
	    GetNumericOption<double>(input, "data_files_size_in_mb", LEROBOT_DEFAULT_DATA_FILE_SIZE_MB);
	result.video_file_size_mb =
	    GetNumericOption<double>(input, "video_files_size_in_mb", LEROBOT_DEFAULT_VIDEO_FILE_SIZE_MB);
	if (chunks_size <= 0 || std::floor(chunks_size) != chunks_size) {
		throw BinderException("LeRobot CHUNKS_SIZE must be a positive integer");
	}
	if (metadata_buffer_size <= 0 || std::floor(metadata_buffer_size) != metadata_buffer_size) {
		throw BinderException("LeRobot METADATA_BUFFER_SIZE must be a positive integer");
	}
	if (!std::isfinite(result.data_file_size_mb) || !std::isfinite(result.video_file_size_mb) ||
	    !(result.data_file_size_mb > 0) || !(result.video_file_size_mb > 0)) {
		throw BinderException("LeRobot data and video file size limits must be positive");
	}
	if (chunks_size > static_cast<double>(NumericLimits<idx_t>::Maximum()) ||
	    metadata_buffer_size > static_cast<double>(NumericLimits<idx_t>::Maximum())) {
		throw BinderException("LeRobot chunk or metadata buffer size is too large");
	}
	result.chunks_size = static_cast<idx_t>(chunks_size);
	result.metadata_buffer_size = static_cast<idx_t>(metadata_buffer_size);

	result.max_visual_frame_bytes = LEROBOT_DEFAULT_MAX_VISUAL_FRAME_BYTES;
	const auto &max_visual_frame_bytes = GetSingleOption(input, "max_visual_frame_bytes", false);
	if (!max_visual_frame_bytes.IsNull()) {
		auto value = max_visual_frame_bytes.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
		if (value == 0 || value > NumericLimits<idx_t>::Maximum()) {
			throw BinderException("LeRobot MAX_VISUAL_FRAME_BYTES must be a positive integer");
		}
		result.max_visual_frame_bytes = static_cast<idx_t>(value);
	}

	const auto &encoder_threads = GetSingleOption(input, "encoder_threads", false);
	if (!encoder_threads.IsNull()) {
		auto value = encoder_threads.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
		if (value == 0 || value > host_threads) {
			throw BinderException("LeRobot ENCODER_THREADS must be between 1 and the DuckDB thread limit (%llu)",
			                      host_threads);
		}
		result.encoder_threads = static_cast<idx_t>(value);
	}

	const auto &video_workers = GetSingleOption(input, "video_workers", false);
	if (!video_workers.IsNull()) {
		auto value = video_workers.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
		if (value == 0 || value > host_threads) {
			throw BinderException("LeRobot VIDEO_WORKERS must be between 1 and the DuckDB thread limit (%llu)",
			                      host_threads);
		}
		result.video_workers = static_cast<idx_t>(value);
	}

	const auto &robot_type = GetSingleOption(input, "robot_type", false);
	if (!robot_type.IsNull()) {
		result.robot_type = StringValue::Get(robot_type.DefaultCastAs(LogicalType::VARCHAR));
		result.has_robot_type = true;
	}
}

void LerobotCopyOptionDefinitions(ClientContext &, CopyOptionsInput &input) {
	input.options["fps"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["features"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	input.options["robot_type"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	input.options["chunks_size"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["data_files_size_in_mb"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	input.options["video_files_size_in_mb"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	input.options["metadata_buffer_size"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["max_visual_frame_bytes"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["video_workers"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["encoder_threads"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["rgb_codec"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	// Preserve fractions until our range/integrality checks have run. BIGINT
	// would let DuckDB round invalid values before the extension sees them.
	input.options["rgb_crf"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	input.options["rgb_gop"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	input.options["depth_min"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	input.options["depth_max"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	input.options["depth_shift"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	input.options["depth_use_log"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
	input.options["depth_clip"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
}

} // namespace duckdb
