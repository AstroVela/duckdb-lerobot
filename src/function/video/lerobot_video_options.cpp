#include "function/lerobot_video_options.hpp"

#include "duckdb/common/exception.hpp"
#include "function/lerobot_temporal.hpp"

#include <algorithm>
#include <cmath>

namespace duckdb {

namespace {

static const idx_t LEROBOT_DEFAULT_DECODE_BATCH_SIZE = 16;
static const idx_t LEROBOT_DEFAULT_TARGET_BUFFER_SIZE = 256;
static const idx_t LEROBOT_DEFAULT_MAX_CACHED_DECODERS = 8;
static const idx_t LEROBOT_DEFAULT_DECODE_THREADS = 8;
static const idx_t LEROBOT_DEFAULT_PRODUCER_THREADS = 4;
static const idx_t LEROBOT_DEFAULT_MAX_OUTPUT_BYTES = 64 * 1024 * 1024;
static const int64_t LEROBOT_DEFAULT_CODEC_THREADS = 1;
static const idx_t LEROBOT_MAX_TARGET_BUFFER_SIZE = 1024 * 1024;
static const idx_t LEROBOT_MAX_CACHED_DECODERS = 1024;
static const idx_t LEROBOT_MAX_DECODE_THREADS = 1024;
static const idx_t LEROBOT_MAX_PRODUCER_THREADS = 1024;
static const idx_t LEROBOT_MAX_PENDING_TARGETS = 10 * 1024 * 1024;
static const double LEROBOT_DEFAULT_CLUSTER_GAP_SECONDS = 10.0;

int64_t GetNamedInteger(TableFunctionBindInput &input, const char *function_name, const char *name,
                        int64_t default_value) {
	auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end()) {
		return default_value;
	}
	if (entry->second.IsNull()) {
		throw BinderException("%s %s must not be NULL", function_name, name);
	}
	return entry->second.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
}

double GetNamedDouble(TableFunctionBindInput &input, const char *function_name, const char *name,
                      double default_value) {
	auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end()) {
		return default_value;
	}
	if (entry->second.IsNull()) {
		throw BinderException("%s %s must not be NULL", function_name, name);
	}
	return entry->second.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
}

string GetNamedString(TableFunctionBindInput &input, const char *function_name, const char *name,
                      const char *default_value) {
	auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end()) {
		return default_value;
	}
	if (entry->second.IsNull()) {
		throw BinderException("%s %s must not be NULL", function_name, name);
	}
	return StringValue::Get(entry->second);
}

} // namespace

LerobotVideoOptions GetLerobotVideoOptions(TableFunctionBindInput &input, const char *function_name) {
	const auto width_value = GetNamedInteger(input, function_name, "width", 0);
	const auto height_value = GetNamedInteger(input, function_name, "height", 0);
	if ((width_value == 0) != (height_value == 0)) {
		throw BinderException("%s width and height must either both be zero or both be positive", function_name);
	}
	if (width_value < 0 || height_value < 0 || width_value > 32768 || height_value > 32768) {
		throw BinderException("%s width and height must be between 0 and 32768", function_name);
	}

	const auto tolerance =
	    GetNamedDouble(input, function_name, "tolerance", LEROBOT_DEFAULT_TEMPORAL_TOLERANCE_SECONDS);
	if (!std::isfinite(tolerance) || tolerance <= 0) {
		throw BinderException("%s tolerance must be finite and positive", function_name);
	}
	const auto cluster_gap = GetNamedDouble(input, function_name, "cluster_gap", LEROBOT_DEFAULT_CLUSTER_GAP_SECONDS);
	if (!std::isfinite(cluster_gap) || cluster_gap < 0) {
		throw BinderException("%s cluster_gap must be finite and non-negative", function_name);
	}
	const auto batch_size_value =
	    GetNamedInteger(input, function_name, "batch_size", LEROBOT_DEFAULT_DECODE_BATCH_SIZE);
	if (batch_size_value <= 0 || batch_size_value > static_cast<int64_t>(STANDARD_VECTOR_SIZE)) {
		throw BinderException("%s batch_size must be between 1 and %d", function_name, STANDARD_VECTOR_SIZE);
	}
	const auto target_buffer_size_value =
	    GetNamedInteger(input, function_name, "target_buffer_size", LEROBOT_DEFAULT_TARGET_BUFFER_SIZE);
	if (target_buffer_size_value <= 0 ||
	    target_buffer_size_value > static_cast<int64_t>(LEROBOT_MAX_TARGET_BUFFER_SIZE)) {
		throw BinderException("%s target_buffer_size must be between 1 and %d", function_name,
		                      LEROBOT_MAX_TARGET_BUFFER_SIZE);
	}
	const auto max_cached_decoders_value =
	    GetNamedInteger(input, function_name, "max_cached_decoders", LEROBOT_DEFAULT_MAX_CACHED_DECODERS);
	if (max_cached_decoders_value <= 0 ||
	    max_cached_decoders_value > static_cast<int64_t>(LEROBOT_MAX_CACHED_DECODERS)) {
		throw BinderException("%s max_cached_decoders must be between 1 and %d", function_name,
		                      LEROBOT_MAX_CACHED_DECODERS);
	}
	const auto decode_threads_value =
	    GetNamedInteger(input, function_name, "decode_threads", LEROBOT_DEFAULT_DECODE_THREADS);
	if (decode_threads_value <= 0 || decode_threads_value > static_cast<int64_t>(LEROBOT_MAX_DECODE_THREADS)) {
		throw BinderException("%s decode_threads must be between 1 and %d", function_name, LEROBOT_MAX_DECODE_THREADS);
	}
	const auto producer_threads_value =
	    GetNamedInteger(input, function_name, "producer_threads", LEROBOT_DEFAULT_PRODUCER_THREADS);
	if (producer_threads_value <= 0 || producer_threads_value > static_cast<int64_t>(LEROBOT_MAX_PRODUCER_THREADS)) {
		throw BinderException("%s producer_threads must be between 1 and %d", function_name,
		                      LEROBOT_MAX_PRODUCER_THREADS);
	}
	const auto max_pending_targets_value = GetNamedInteger(input, function_name, "max_pending_targets", 0);
	if (max_pending_targets_value < 0 ||
	    max_pending_targets_value > static_cast<int64_t>(LEROBOT_MAX_PENDING_TARGETS)) {
		throw BinderException("%s max_pending_targets must be between 0 and %d", function_name,
		                      LEROBOT_MAX_PENDING_TARGETS);
	}
	idx_t max_pending_targets;
	if (max_pending_targets_value == 0) {
		max_pending_targets =
		    static_cast<idx_t>(target_buffer_size_value) * static_cast<idx_t>(decode_threads_value) * 2;
		max_pending_targets = std::min<idx_t>(max_pending_targets, LEROBOT_MAX_PENDING_TARGETS);
	} else {
		max_pending_targets = static_cast<idx_t>(max_pending_targets_value);
	}

	const auto max_output_bytes_value = GetNamedInteger(input, function_name, "max_output_bytes",
	                                                    static_cast<int64_t>(LEROBOT_DEFAULT_MAX_OUTPUT_BYTES));
	if (max_output_bytes_value <= 0) {
		throw BinderException("%s max_output_bytes must be positive", function_name);
	}
	const auto codec_threads_value =
	    GetNamedInteger(input, function_name, "codec_threads", LEROBOT_DEFAULT_CODEC_THREADS);
	if (codec_threads_value < 0 || codec_threads_value > 64) {
		throw BinderException("%s codec_threads must be between 0 and 64", function_name);
	}
	const auto depth_output_unit_value = GetNamedString(input, function_name, "depth_output_unit", "mm");
	LerobotDepthOutputUnit depth_output_unit;
	if (depth_output_unit_value == "m") {
		depth_output_unit = LerobotDepthOutputUnit::METERS;
	} else if (depth_output_unit_value == "mm") {
		depth_output_unit = LerobotDepthOutputUnit::MILLIMETERS;
	} else {
		throw BinderException("%s depth_output_unit must be 'm' or 'mm'", function_name);
	}

	LerobotVideoOptions result;
	result.tolerance = tolerance;
	result.cluster_gap = cluster_gap;
	result.width = static_cast<int32_t>(width_value);
	result.height = static_cast<int32_t>(height_value);
	result.output_batch_size = static_cast<idx_t>(batch_size_value);
	result.target_buffer_size = static_cast<idx_t>(target_buffer_size_value);
	result.max_cached_decoders = static_cast<idx_t>(max_cached_decoders_value);
	result.decode_threads = static_cast<idx_t>(decode_threads_value);
	result.producer_threads = static_cast<idx_t>(producer_threads_value);
	result.max_pending_targets = max_pending_targets;
	result.max_output_bytes = static_cast<idx_t>(max_output_bytes_value);
	result.codec_threads = static_cast<int32_t>(codec_threads_value);
	result.depth_output_unit = depth_output_unit;
	return result;
}

void AddLerobotVideoOptionParameters(TableFunction &function, bool include_video_keys, bool source_function) {
	if (include_video_keys) {
		function.named_parameters["video_keys"] = LogicalType::LIST(LogicalType::VARCHAR);
	}
	function.named_parameters["width"] = LogicalType::BIGINT;
	function.named_parameters["height"] = LogicalType::BIGINT;
	function.named_parameters["tolerance"] = LogicalType::DOUBLE;
	function.named_parameters["cluster_gap"] = LogicalType::DOUBLE;
	function.named_parameters["batch_size"] = LogicalType::BIGINT;
	function.named_parameters["target_buffer_size"] = LogicalType::BIGINT;
	function.named_parameters["max_cached_decoders"] = LogicalType::BIGINT;
	function.named_parameters["decode_threads"] = LogicalType::BIGINT;
	if (source_function) {
		function.named_parameters["producer_threads"] = LogicalType::BIGINT;
	}
	function.named_parameters["max_pending_targets"] = LogicalType::BIGINT;
	function.named_parameters["max_output_bytes"] = LogicalType::BIGINT;
	function.named_parameters["codec_threads"] = LogicalType::BIGINT;
	function.named_parameters["depth_output_unit"] = LogicalType::VARCHAR;
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
}

} // namespace duckdb
