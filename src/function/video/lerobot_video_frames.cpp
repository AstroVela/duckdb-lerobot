#include "function/lerobot_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/table_filter.hpp"

#include "function/lerobot_multi_file_reader.hpp"
#include "function/lerobot_temporal.hpp"
#include "storage/lerobot_metadata_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <utility>

#ifdef LEROBOT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
#endif

namespace duckdb {

namespace {

static const idx_t LEROBOT_DEFAULT_DECODE_BATCH_SIZE = 16;
static const idx_t LEROBOT_DEFAULT_TARGET_BUFFER_SIZE = 256;
static const idx_t LEROBOT_DEFAULT_MAX_CACHED_DECODERS = 8;
static const idx_t LEROBOT_DEFAULT_DECODE_THREADS = 8;
static const idx_t LEROBOT_DEFAULT_MAX_OUTPUT_BYTES = 64 * 1024 * 1024;
static const int64_t LEROBOT_DEFAULT_CODEC_THREADS = 1;
static const idx_t LEROBOT_MAX_TARGET_BUFFER_SIZE = 1024 * 1024;
static const idx_t LEROBOT_MAX_CACHED_DECODERS = 1024;
static const idx_t LEROBOT_MAX_DECODE_THREADS = 1024;
static const idx_t LEROBOT_MAX_PENDING_TARGETS = 10 * 1024 * 1024;
static const idx_t LEROBOT_MAX_WINDOW_TARGETS = 100000;
static const idx_t LEROBOT_DECODE_FRAME_BUDGET = 20000;
static const double LEROBOT_DEFAULT_CLUSTER_GAP_SECONDS = 10.0;
#ifdef LEROBOT_HAVE_FFMPEG
static const uint16_t LEROBOT_DEPTH_QMAX = 4095;
#endif

enum class LerobotDepthOutputUnit : uint8_t { METERS, MILLIMETERS };

template <typename T>
T *GetMutableFlatData(Vector &vector) {
	return FlatVector::GetData<T>(vector);
}

void PrepareUnifiedFormat(Vector &vector, idx_t count, UnifiedVectorFormat &format) {
	vector.ToUnifiedFormat(count, format);
}

void SetOutputCardinality(DataChunk &output, idx_t count) {
	output.SetCardinality(count);
}

void CheckForInterrupt(ClientContext &context) {
	if (context.IsInterrupted()) {
		throw InterruptException();
	}
}

bool GetRefreshParameter(TableFunctionBindInput &input, const char *function_name) {
	auto entry = input.named_parameters.find("refresh");
	if (entry == input.named_parameters.end()) {
		return false;
	}
	if (entry->second.IsNull()) {
		throw BinderException("%s refresh must not be NULL", function_name);
	}
	return BooleanValue::Get(entry->second);
}

vector<int64_t> GetNonNegativeIndices(const Value &value, const char *parameter_name) {
	if (value.IsNull()) {
		throw BinderException("lerobot_video_frames %s must not be NULL", parameter_name);
	}
	vector<int64_t> result;
	for (const auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw BinderException("lerobot_video_frames %s must not contain NULL", parameter_name);
		}
		auto index = child.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		if (index < 0) {
			throw BinderException("LeRobot %s must be non-negative", parameter_name);
		}
		result.push_back(index);
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

vector<string> GetVideoKeys(TableFunctionBindInput &input, const LerobotVideoMetadata &metadata) {
	auto entry = input.named_parameters.find("video_keys");
	if (entry == input.named_parameters.end()) {
		return metadata.GetVideoKeys();
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_video_frames video_keys must not be NULL");
	}
	vector<string> result;
	for (const auto &child : ListValue::GetChildren(entry->second)) {
		if (child.IsNull()) {
			throw BinderException("lerobot_video_frames video_keys must not contain NULL");
		}
		auto key = StringValue::Get(child);
		if (key.empty()) {
			throw BinderException("lerobot_video_frames video_keys must not contain an empty key");
		}
		result.push_back(std::move(key));
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

int64_t GetNamedInteger(TableFunctionBindInput &input, const char *name, int64_t default_value) {
	auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end()) {
		return default_value;
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_video_frames %s must not be NULL", name);
	}
	return entry->second.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
}

double GetNamedDouble(TableFunctionBindInput &input, const char *name, double default_value) {
	auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end()) {
		return default_value;
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_video_frames %s must not be NULL", name);
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

string ValueListSQL(const vector<string> &values) {
	string result = "[";
	for (idx_t index = 0; index < values.size(); index++) {
		if (index > 0) {
			result += ", ";
		}
		result += Value(values[index]).ToSQLString();
	}
	result += "]";
	return result;
}

string IntegerListSQL(const vector<int64_t> &values) {
	string result = "(";
	for (idx_t index = 0; index < values.size(); index++) {
		if (index > 0) {
			result += ", ";
		}
		result += std::to_string(values[index]);
	}
	result += ")";
	return result;
}

enum LerobotVideoFrameColumn {
	LEROBOT_FRAME_EPISODE_INDEX = 0,
	LEROBOT_FRAME_FRAME_INDEX = 1,
	LEROBOT_FRAME_TIMESTAMP = 2,
	LEROBOT_FRAME_VIDEO_KEY = 3,
	LEROBOT_FRAME_VIDEO_PATH = 4,
	LEROBOT_FRAME_VIDEO_TIMESTAMP = 5,
	LEROBOT_FRAME_DECODED_TIMESTAMP = 6,
	LEROBOT_FRAME_WIDTH = 7,
	LEROBOT_FRAME_HEIGHT = 8,
	LEROBOT_FRAME_CHANNELS = 9,
	LEROBOT_FRAME_IMAGE = 10,
	LEROBOT_FRAME_COLUMN_COUNT = 11
};

enum LerobotVideoWindowColumn {
	LEROBOT_WINDOW_REQUEST_ID = 0,
	LEROBOT_WINDOW_REQUEST_ORDINAL = 1,
	LEROBOT_WINDOW_DELTA_ORDINAL = 2,
	LEROBOT_WINDOW_DELTA_TIMESTAMP = 3,
	LEROBOT_WINDOW_DELTA_FRAME_OFFSET = 4,
	LEROBOT_WINDOW_IS_PADDING = 5,
	LEROBOT_WINDOW_EPISODE_INDEX = 6,
	LEROBOT_WINDOW_FRAME_INDEX = 7,
	LEROBOT_WINDOW_TARGET_FRAME_INDEX = 8,
	LEROBOT_WINDOW_TIMESTAMP = 9,
	LEROBOT_WINDOW_VIDEO_KEY = 10,
	LEROBOT_WINDOW_VIDEO_PATH = 11,
	LEROBOT_WINDOW_VIDEO_TIMESTAMP = 12,
	LEROBOT_WINDOW_DECODED_TIMESTAMP = 13,
	LEROBOT_WINDOW_WIDTH = 14,
	LEROBOT_WINDOW_HEIGHT = 15,
	LEROBOT_WINDOW_CHANNELS = 16,
	LEROBOT_WINDOW_IMAGE = 17,
	LEROBOT_WINDOW_COLUMN_COUNT = 18
};

enum LerobotVideoTargetColumn {
	LEROBOT_TARGET_REQUEST_ID = 0,
	LEROBOT_TARGET_ORDINAL = 1,
	LEROBOT_TARGET_EPISODE_INDEX = 2,
	LEROBOT_TARGET_FRAME_INDEX = 3,
	LEROBOT_TARGET_VIDEO_KEY = 4,
	LEROBOT_TARGET_DELTA_INDEX = 5,
	LEROBOT_TARGET_DELTA_TIMESTAMP = 6,
	LEROBOT_TARGET_DELTA_FRAME_OFFSET = 7,
	LEROBOT_TARGET_IS_PADDING = 8,
	LEROBOT_TARGET_FRAME_INDEX_RESOLVED = 9,
	LEROBOT_TARGET_TIMESTAMP = 10,
	LEROBOT_TARGET_VIDEO_PATH = 11,
	LEROBOT_TARGET_VIDEO_TIMESTAMP = 12,
	LEROBOT_TARGET_DECODED_TIMESTAMP = 13,
	LEROBOT_TARGET_WIDTH = 14,
	LEROBOT_TARGET_HEIGHT = 15,
	LEROBOT_TARGET_CHANNELS = 16,
	LEROBOT_TARGET_IMAGE = 17,
	LEROBOT_TARGET_COLUMN_COUNT = 18
};

struct LerobotVideoOptions {
	double tolerance;
	double cluster_gap;
	int32_t width;
	int32_t height;
	idx_t output_batch_size;
	idx_t target_buffer_size;
	idx_t max_cached_decoders;
	idx_t decode_threads;
	idx_t max_pending_targets;
	idx_t max_output_bytes;
	int32_t codec_threads;
	LerobotDepthOutputUnit depth_output_unit;
};

LerobotVideoOptions GetVideoOptions(TableFunctionBindInput &input, const char *function_name) {
	const auto width_value = GetNamedInteger(input, "width", 0);
	const auto height_value = GetNamedInteger(input, "height", 0);
	if ((width_value == 0) != (height_value == 0)) {
		throw BinderException("%s width and height must either both be zero or both be positive", function_name);
	}
	if (width_value < 0 || height_value < 0 || width_value > 32768 || height_value > 32768) {
		throw BinderException("%s width and height must be between 0 and 32768", function_name);
	}

	const auto tolerance = GetNamedDouble(input, "tolerance", LEROBOT_DEFAULT_TEMPORAL_TOLERANCE_SECONDS);
	if (!std::isfinite(tolerance) || tolerance <= 0) {
		throw BinderException("%s tolerance must be finite and positive", function_name);
	}
	const auto cluster_gap = GetNamedDouble(input, "cluster_gap", LEROBOT_DEFAULT_CLUSTER_GAP_SECONDS);
	if (!std::isfinite(cluster_gap) || cluster_gap < 0) {
		throw BinderException("%s cluster_gap must be finite and non-negative", function_name);
	}
	const auto batch_size_value = GetNamedInteger(input, "batch_size", LEROBOT_DEFAULT_DECODE_BATCH_SIZE);
	if (batch_size_value <= 0 || batch_size_value > static_cast<int64_t>(STANDARD_VECTOR_SIZE)) {
		throw BinderException("%s batch_size must be between 1 and %d", function_name, STANDARD_VECTOR_SIZE);
	}
	const auto target_buffer_size_value =
	    GetNamedInteger(input, "target_buffer_size", LEROBOT_DEFAULT_TARGET_BUFFER_SIZE);
	if (target_buffer_size_value <= 0 ||
	    target_buffer_size_value > static_cast<int64_t>(LEROBOT_MAX_TARGET_BUFFER_SIZE)) {
		throw BinderException("%s target_buffer_size must be between 1 and %d", function_name,
		                      LEROBOT_MAX_TARGET_BUFFER_SIZE);
	}
	const auto cached_decoders_entry = input.named_parameters.find("max_cached_decoders");
	const auto legacy_open_shards_entry = input.named_parameters.find("max_open_shards");
	const auto max_cached_decoders_value = GetNamedInteger(
	    input, cached_decoders_entry != input.named_parameters.end() ? "max_cached_decoders" : "max_open_shards",
	    LEROBOT_DEFAULT_MAX_CACHED_DECODERS);
	if (cached_decoders_entry != input.named_parameters.end() &&
	    legacy_open_shards_entry != input.named_parameters.end()) {
		const auto legacy_value = GetNamedInteger(input, "max_open_shards", LEROBOT_DEFAULT_MAX_CACHED_DECODERS);
		if (legacy_value != max_cached_decoders_value) {
			throw BinderException("%s max_cached_decoders and deprecated max_open_shards must match when both are set",
			                      function_name);
		}
	}
	if (max_cached_decoders_value <= 0 ||
	    max_cached_decoders_value > static_cast<int64_t>(LEROBOT_MAX_CACHED_DECODERS)) {
		const auto parameter_name =
		    cached_decoders_entry != input.named_parameters.end() ? "max_cached_decoders" : "max_open_shards";
		throw BinderException("%s %s must be between 1 and %d", function_name, parameter_name,
		                      LEROBOT_MAX_CACHED_DECODERS);
	}
	const auto decode_threads_value = GetNamedInteger(input, "decode_threads", LEROBOT_DEFAULT_DECODE_THREADS);
	if (decode_threads_value <= 0 || decode_threads_value > static_cast<int64_t>(LEROBOT_MAX_DECODE_THREADS)) {
		throw BinderException("%s decode_threads must be between 1 and %d", function_name, LEROBOT_MAX_DECODE_THREADS);
	}
	const auto max_pending_targets_value = GetNamedInteger(input, "max_pending_targets", 0);
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

	const auto max_output_bytes_value =
	    GetNamedInteger(input, "max_output_bytes", static_cast<int64_t>(LEROBOT_DEFAULT_MAX_OUTPUT_BYTES));
	if (max_output_bytes_value <= 0) {
		throw BinderException("%s max_output_bytes must be positive", function_name);
	}
	const auto codec_threads_value = GetNamedInteger(input, "codec_threads", LEROBOT_DEFAULT_CODEC_THREADS);
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
	result.max_pending_targets = max_pending_targets;
	result.max_output_bytes = static_cast<idx_t>(max_output_bytes_value);
	result.codec_threads = static_cast<int32_t>(codec_threads_value);
	result.depth_output_unit = depth_output_unit;
	return result;
}

struct LerobotDecodeTarget {
	LerobotDecodeTarget(int64_t episode_index_p, int64_t frame_index_p, double frame_timestamp_p,
	                    double video_timestamp_p, idx_t route_index_p)
	    : request_id(0), request_ordinal(0), delta_ordinal(0), delta_timestamp(0), delta_frame_offset(0),
	      is_padding(false), episode_index(episode_index_p), frame_index(frame_index_p),
	      target_frame_index(frame_index_p), frame_timestamp(frame_timestamp_p), video_timestamp(video_timestamp_p),
	      route_index(route_index_p) {
	}

	LerobotDecodeTarget(int64_t request_id_p, idx_t request_ordinal_p, idx_t delta_ordinal_p, double delta_timestamp_p,
	                    int64_t delta_frame_offset_p, bool is_padding_p, int64_t episode_index_p, int64_t frame_index_p,
	                    int64_t target_frame_index_p, double frame_timestamp_p, double video_timestamp_p,
	                    idx_t route_index_p)
	    : request_id(request_id_p), request_ordinal(request_ordinal_p), delta_ordinal(delta_ordinal_p),
	      delta_timestamp(delta_timestamp_p), delta_frame_offset(delta_frame_offset_p), is_padding(is_padding_p),
	      episode_index(episode_index_p), frame_index(frame_index_p), target_frame_index(target_frame_index_p),
	      frame_timestamp(frame_timestamp_p), video_timestamp(video_timestamp_p), route_index(route_index_p) {
	}

	int64_t request_id;
	idx_t request_ordinal;
	idx_t delta_ordinal;
	double delta_timestamp;
	int64_t delta_frame_offset;
	bool is_padding;
	int64_t episode_index;
	int64_t frame_index;
	int64_t target_frame_index;
	double frame_timestamp;
	double video_timestamp;
	idx_t route_index;
};

//! Query-local counters exposed through DuckDB's JSON/query profiler. Atomics
//! allow decoder workers to update the counters without serializing hot paths.
struct LerobotVideoDecodeMetrics {
	LerobotVideoDecodeMetrics()
	    : targets(0), decoder_acquires(0), decoder_cache_hits(0), decoder_opens(0), decoder_evictions(0),
	      decoder_seeks(0), avio_seeks(0), video_bytes_read(0), frames_decoded(0), rgb_conversions(0),
	      rgb_fanout_hits(0), depth_conversions(0), depth_fanout_hits(0) {
	}

	std::atomic<uint64_t> targets;
	std::atomic<uint64_t> decoder_acquires;
	std::atomic<uint64_t> decoder_cache_hits;
	std::atomic<uint64_t> decoder_opens;
	std::atomic<uint64_t> decoder_evictions;
	std::atomic<uint64_t> decoder_seeks;
	std::atomic<uint64_t> avio_seeks;
	std::atomic<uint64_t> video_bytes_read;
	std::atomic<uint64_t> frames_decoded;
	std::atomic<uint64_t> rgb_conversions;
	std::atomic<uint64_t> rgb_fanout_hits;
	std::atomic<uint64_t> depth_conversions;
	std::atomic<uint64_t> depth_fanout_hits;
};

struct LerobotDecodeBuffer {
	idx_t shard_index;
	vector<LerobotDecodeTarget> targets;
	vector<vector<idx_t>> clusters;
};

struct LerobotVideoFramesBindData final : public TableFunctionData {
	LerobotVideoFramesBindData(shared_ptr<LerobotVideoMetadata> metadata_p, vector<LerobotVideoRoute> routes_p,
	                           string frame_query_p, bool window_mode_p, const LerobotVideoOptions &options)
	    : metadata(std::move(metadata_p)), routes(std::move(routes_p)), frame_query(std::move(frame_query_p)),
	      window_mode(window_mode_p), tolerance(options.tolerance), cluster_gap(options.cluster_gap),
	      width(options.width), height(options.height), output_batch_size(options.output_batch_size),
	      target_buffer_size(options.target_buffer_size), max_cached_decoders(options.max_cached_decoders),
	      decode_threads(options.decode_threads), max_pending_targets(options.max_pending_targets),
	      max_output_bytes(options.max_output_bytes), codec_threads(options.codec_threads),
	      depth_output_unit(options.depth_output_unit) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotVideoFramesBindData>(metadata, routes, frame_query, window_mode, GetOptions());
	}

	LerobotVideoOptions GetOptions() const {
		LerobotVideoOptions options;
		options.tolerance = tolerance;
		options.cluster_gap = cluster_gap;
		options.width = width;
		options.height = height;
		options.output_batch_size = output_batch_size;
		options.target_buffer_size = target_buffer_size;
		options.max_cached_decoders = max_cached_decoders;
		options.decode_threads = decode_threads;
		options.max_pending_targets = max_pending_targets;
		options.max_output_bytes = max_output_bytes;
		options.codec_threads = codec_threads;
		options.depth_output_unit = depth_output_unit;
		return options;
	}

	shared_ptr<LerobotVideoMetadata> metadata;
	vector<LerobotVideoRoute> routes;
	string frame_query;
	bool window_mode;
	double tolerance;
	double cluster_gap;
	int32_t width;
	int32_t height;
	idx_t output_batch_size;
	idx_t target_buffer_size;
	idx_t max_cached_decoders;
	idx_t decode_threads;
	idx_t max_pending_targets;
	idx_t max_output_bytes;
	int32_t codec_threads;
	LerobotDepthOutputUnit depth_output_unit;
};

void FinalizeDecodeBuffer(LerobotDecodeBuffer &buffer, double cluster_gap) {
	std::sort(buffer.targets.begin(), buffer.targets.end(),
	          [](const LerobotDecodeTarget &left, const LerobotDecodeTarget &right) {
		          if (left.video_timestamp != right.video_timestamp) {
			          return left.video_timestamp < right.video_timestamp;
		          }
		          if (left.episode_index != right.episode_index) {
			          return left.episode_index < right.episode_index;
		          }
		          if (left.frame_index != right.frame_index) {
			          return left.frame_index < right.frame_index;
		          }
		          if (left.route_index != right.route_index) {
			          return left.route_index < right.route_index;
		          }
		          if (left.request_ordinal != right.request_ordinal) {
			          return left.request_ordinal < right.request_ordinal;
		          }
		          return left.delta_ordinal < right.delta_ordinal;
	          });
	for (idx_t target_index = 0; target_index < buffer.targets.size(); target_index++) {
		bool new_cluster = buffer.clusters.empty();
		if (!new_cluster) {
			const auto previous_index = buffer.clusters.back().back();
			new_cluster =
			    buffer.targets[target_index].video_timestamp - buffer.targets[previous_index].video_timestamp >
			    cluster_gap;
		}
		if (new_cluster) {
			buffer.clusters.push_back(vector<idx_t>());
		}
		buffer.clusters.back().push_back(target_index);
	}
}

unique_ptr<FunctionData> LerobotVideoFramesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_video_frames root must not be NULL");
	}
	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	auto episode_indices = GetNonNegativeIndices(input.inputs[1], "episode_indices");
	const auto refresh = GetRefreshParameter(input, "lerobot_video_frames");

	bool cache_hit;
	auto video_metadata = LerobotVideoMetadata::Get(context, root, refresh, cache_hit);
	auto video_keys = GetVideoKeys(input, *video_metadata);
	auto routes = video_metadata->ResolveRoutes(episode_indices, video_keys);
	auto options = GetVideoOptions(input, "lerobot_video_frames");

	vector<int64_t> frame_indices;
	auto frame_filter = input.named_parameters.find("frame_indices");
	if (frame_filter != input.named_parameters.end()) {
		frame_indices = GetNonNegativeIndices(frame_filter->second, "frame_indices");
	}

	names = {"episode_index",     "frame_index", "timestamp", "video_key", "video_path", "video_timestamp",
	         "decoded_timestamp", "width",       "height",    "channels",  "image"};
	return_types = {LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::DOUBLE, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::DOUBLE,  LogicalType::DOUBLE, LogicalType::INTEGER,
	                LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::BLOB};

	string frame_query;
	if (!episode_indices.empty() && !routes.empty() &&
	    (frame_filter == input.named_parameters.end() || !frame_indices.empty())) {
		bool data_cache_hit;
		// LerobotVideoMetadata::Get above has already refreshed and validated the
		// shared base metadata cache when refresh was requested.
		auto dataset_metadata = LerobotDatasetMetadata::Get(context, root, false, data_cache_hit);
		auto data_files = dataset_metadata->ResolveDataFiles(episode_indices);
		if (!data_files.empty()) {
			frame_query = "SELECT CAST(episode_index AS BIGINT), CAST(frame_index AS BIGINT), "
			              "CAST(timestamp AS DOUBLE) FROM read_parquet(" +
			              ValueListSQL(data_files) + ") WHERE episode_index IN " + IntegerListSQL(episode_indices);
			if (frame_filter != input.named_parameters.end()) {
				frame_query += " AND frame_index IN " + IntegerListSQL(frame_indices);
			}
		}
	}

	return make_uniq<LerobotVideoFramesBindData>(std::move(video_metadata), std::move(routes), std::move(frame_query),
	                                             false, options);
}

struct LerobotVideoWindowRequest {
	int64_t request_id;
	idx_t request_ordinal;
	int64_t episode_index;
	int64_t frame_index;
};

vector<LerobotVideoWindowRequest> GetVideoWindowRequests(const Value &value) {
	if (value.IsNull()) {
		throw BinderException("lerobot_video_windows requests must not be NULL");
	}
	vector<LerobotVideoWindowRequest> result;
	const auto &requests = ListValue::GetChildren(value);
	result.reserve(requests.size());
	for (idx_t request_ordinal = 0; request_ordinal < requests.size(); request_ordinal++) {
		if (requests[request_ordinal].IsNull()) {
			throw BinderException("lerobot_video_windows requests must not contain NULL");
		}
		const auto &fields = StructValue::GetChildren(requests[request_ordinal]);
		if (fields.size() != 3 || fields[0].IsNull() || fields[1].IsNull() || fields[2].IsNull()) {
			throw BinderException("lerobot_video_windows request fields must not be NULL");
		}
		LerobotVideoWindowRequest request;
		request.request_id = fields[0].DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		request.request_ordinal = request_ordinal;
		request.episode_index = fields[1].DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		request.frame_index = fields[2].DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		if (request.episode_index < 0 || request.frame_index < 0) {
			throw BinderException("lerobot_video_windows episode_index and frame_index must be non-negative");
		}
		result.push_back(request);
	}
	return result;
}

string BuildVideoWindowQuery(const vector<LerobotVideoWindowRequest> &requests,
                             const vector<LerobotTemporalDelta> &deltas,
                             const unordered_map<int64_t, int64_t> &episode_lengths, const vector<string> &data_files,
                             const vector<int64_t> &episode_indices) {
	if (requests.empty() || deltas.empty() || data_files.empty()) {
		return string();
	}

	vector<int64_t> target_frame_indices;
	string values;
	for (idx_t request_index = 0; request_index < requests.size(); request_index++) {
		const auto &request = requests[request_index];
		auto length_entry = episode_lengths.find(request.episode_index);
		D_ASSERT(length_entry != episode_lengths.end());
		const auto last_frame_index = length_entry->second - 1;
		for (idx_t delta_index = 0; delta_index < deltas.size(); delta_index++) {
			const auto &delta = deltas[delta_index];
			int64_t target_frame_index;
			bool is_padding = false;
			if (delta.frame_offset < -request.frame_index) {
				target_frame_index = 0;
				is_padding = true;
			} else if (delta.frame_offset > last_frame_index - request.frame_index) {
				target_frame_index = last_frame_index;
				is_padding = true;
			} else {
				target_frame_index = request.frame_index + delta.frame_offset;
			}
			if (!values.empty()) {
				values += ", ";
			}
			values += "(" + std::to_string(request.request_id) + ", " +
			          std::to_string(static_cast<uint64_t>(request.request_ordinal)) + ", " +
			          std::to_string(static_cast<uint64_t>(delta_index)) + ", " +
			          Value::DOUBLE(delta.timestamp).ToSQLString() + ", " + std::to_string(delta.frame_offset) + ", " +
			          (is_padding ? "TRUE" : "FALSE") + ", " + std::to_string(request.episode_index) + ", " +
			          std::to_string(request.frame_index) + ", " + std::to_string(target_frame_index) + ")";
			target_frame_indices.push_back(target_frame_index);
		}
	}
	std::sort(target_frame_indices.begin(), target_frame_indices.end());
	target_frame_indices.erase(std::unique(target_frame_indices.begin(), target_frame_indices.end()),
	                           target_frame_indices.end());

	string query =
	    "WITH requested(request_id, request_ordinal, delta_ordinal, delta_timestamp, delta_frame_offset, "
	    "is_padding, episode_index, frame_index, target_frame_index) AS (VALUES " +
	    values +
	    ") SELECT CAST(requested.request_id AS BIGINT), CAST(requested.request_ordinal AS BIGINT), "
	    "CAST(requested.delta_ordinal AS BIGINT), CAST(requested.delta_timestamp AS DOUBLE), "
	    "CAST(requested.delta_frame_offset AS BIGINT), CAST(requested.is_padding AS BOOLEAN), "
	    "CAST(requested.episode_index AS BIGINT), CAST(requested.frame_index AS BIGINT), "
	    "CAST(requested.target_frame_index AS BIGINT), CAST(frames.timestamp AS DOUBLE), "
	    "CAST(count(frames.frame_index) OVER (PARTITION BY requested.request_ordinal, requested.delta_ordinal) "
	    "AS BIGINT) FROM requested LEFT JOIN (SELECT episode_index, frame_index, timestamp FROM read_parquet(" +
	    ValueListSQL(data_files) + ") WHERE episode_index IN " + IntegerListSQL(episode_indices) +
	    " AND frame_index IN " + IntegerListSQL(target_frame_indices) +
	    ") frames ON frames.episode_index = requested.episode_index AND "
	    "frames.frame_index = requested.target_frame_index ORDER BY requested.request_ordinal, "
	    "requested.delta_ordinal";
	return query;
}

unique_ptr<FunctionData> LerobotVideoWindowsBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_video_windows root must not be NULL");
	}
	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	auto requests = GetVideoWindowRequests(input.inputs[1]);

	bool cache_hit;
	auto video_metadata =
	    LerobotVideoMetadata::Get(context, root, GetRefreshParameter(input, "lerobot_video_windows"), cache_hit);
	auto video_keys = GetVideoKeys(input, *video_metadata);
	auto options = GetVideoOptions(input, "lerobot_video_windows");
	auto deltas = GetLerobotTemporalDeltas(input, video_metadata->GetFPS(), options.tolerance, "lerobot_video_windows");
	if (!deltas.empty() && requests.size() > LEROBOT_MAX_WINDOW_TARGETS / deltas.size()) {
		throw BinderException("lerobot_video_windows expands to more than %d frame targets",
		                      LEROBOT_MAX_WINDOW_TARGETS);
	}

	vector<int64_t> episode_indices;
	episode_indices.reserve(requests.size());
	for (const auto &request : requests) {
		episode_indices.push_back(request.episode_index);
	}
	std::sort(episode_indices.begin(), episode_indices.end());
	episode_indices.erase(std::unique(episode_indices.begin(), episode_indices.end()), episode_indices.end());
	auto routes = video_metadata->ResolveRoutes(episode_indices, video_keys);

	unordered_map<int64_t, int64_t> episode_lengths;
	unordered_map<int64_t, idx_t> episode_route_counts;
	for (const auto &route : routes) {
		auto length_entry = episode_lengths.find(route.episode_index);
		if (length_entry != episode_lengths.end() && length_entry->second != route.episode_length) {
			throw BinderException("LeRobot video routes disagree on the length of episode %d", route.episode_index);
		}
		episode_lengths[route.episode_index] = route.episode_length;
		episode_route_counts[route.episode_index]++;
	}
	if (!video_keys.empty()) {
		for (const auto episode_index : episode_indices) {
			if (episode_route_counts[episode_index] != video_keys.size()) {
				throw BinderException("LeRobot episode %d does not have every requested video route", episode_index);
			}
		}
		for (const auto &request : requests) {
			const auto episode_length = episode_lengths[request.episode_index];
			if (request.frame_index >= episode_length) {
				throw BinderException("LeRobot frame %d is outside episode %d length %d", request.frame_index,
				                      request.episode_index, episode_length);
			}
		}
	}

	names = {"request_id",      "request_ordinal",    "delta_ordinal",
	         "delta_timestamp", "delta_frame_offset", "is_padding",
	         "episode_index",   "frame_index",        "target_frame_index",
	         "timestamp",       "video_key",          "video_path",
	         "video_timestamp", "decoded_timestamp",  "width",
	         "height",          "channels",           "image"};
	return_types = {LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::DOUBLE,
	                LogicalType::BIGINT,  LogicalType::BOOLEAN, LogicalType::BIGINT,  LogicalType::BIGINT,
	                LogicalType::BIGINT,  LogicalType::DOUBLE,  LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::INTEGER, LogicalType::INTEGER,
	                LogicalType::INTEGER, LogicalType::BLOB};

	string frame_query;
	if (!requests.empty() && !deltas.empty() && !routes.empty()) {
		bool data_cache_hit;
		auto dataset_metadata = LerobotDatasetMetadata::Get(context, root, false, data_cache_hit);
		auto data_files = dataset_metadata->ResolveDataFiles(episode_indices);
		frame_query = BuildVideoWindowQuery(requests, deltas, episode_lengths, data_files, episode_indices);
	}
	return make_uniq<LerobotVideoFramesBindData>(std::move(video_metadata), std::move(routes), std::move(frame_query),
	                                             true, options);
}

struct LerobotVideoTargetsBindData final : public TableFunctionData {
	LerobotVideoTargetsBindData(shared_ptr<LerobotDatasetMetadata> dataset_p,
	                            shared_ptr<LerobotVideoMetadata> metadata_p, vector<LerobotTemporalDelta> deltas_p,
	                            vector<idx_t> input_columns_p, const LerobotVideoOptions &options_p)
	    : dataset(std::move(dataset_p)), metadata(std::move(metadata_p)), deltas(std::move(deltas_p)),
	      input_columns(std::move(input_columns_p)), options(options_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotVideoTargetsBindData>(dataset, metadata, deltas, input_columns, options);
	}

	shared_ptr<LerobotDatasetMetadata> dataset;
	shared_ptr<LerobotVideoMetadata> metadata;
	vector<LerobotTemporalDelta> deltas;
	//! Physical input indexes in request_id, episode_index, frame_index,
	//! video_key, delta_index order.
	vector<idx_t> input_columns;
	LerobotVideoOptions options;
};

idx_t FindTargetInputColumn(TableFunctionBindInput &input, const char *name) {
	optional_idx result;
	for (idx_t column = 0; column < input.input_table_names.size(); column++) {
		if (input.input_table_names[column] != name) {
			continue;
		}
		if (result.IsValid()) {
			throw BinderException("lerobot_video_targets input relation contains duplicate column '%s'", name);
		}
		result = column;
	}
	if (!result.IsValid()) {
		throw BinderException("lerobot_video_targets input relation requires column '%s'", name);
	}
	return result.GetIndex();
}

unique_ptr<FunctionData> LerobotVideoTargetsBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("lerobot_video_targets root must not be NULL");
	}
	if (input.input_table_types.size() != 5 || input.input_table_names.size() != 5) {
		throw BinderException("lerobot_video_targets input relation must contain exactly request_id, episode_index, "
		                      "frame_index, video_key, and delta_index");
	}
	vector<idx_t> input_columns;
	input_columns.push_back(FindTargetInputColumn(input, "request_id"));
	input_columns.push_back(FindTargetInputColumn(input, "episode_index"));
	input_columns.push_back(FindTargetInputColumn(input, "frame_index"));
	input_columns.push_back(FindTargetInputColumn(input, "video_key"));
	input_columns.push_back(FindTargetInputColumn(input, "delta_index"));
	const LogicalType required_types[] = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                                      LogicalType::VARCHAR, LogicalType::BIGINT};
	for (idx_t required = 0; required < input_columns.size(); required++) {
		// Let DuckDB add a typed projection/cast between the input relation and
		// this operator. Execution can then consume vectors without Value boxing.
		input.input_table_types[input_columns[required]] = required_types[required];
	}

	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	bool video_cache_hit;
	auto metadata =
	    LerobotVideoMetadata::Get(context, root, GetRefreshParameter(input, "lerobot_video_targets"), video_cache_hit);
	bool dataset_cache_hit;
	auto dataset = LerobotDatasetMetadata::Get(context, root, false, dataset_cache_hit);
	auto options = GetVideoOptions(input, "lerobot_video_targets");
	auto deltas = GetLerobotTemporalDeltas(input, metadata->GetFPS(), options.tolerance, "lerobot_video_targets");

	names = {"request_id",      "target_ordinal",
	         "episode_index",   "frame_index",
	         "video_key",       "delta_index",
	         "delta_timestamp", "delta_frame_offset",
	         "is_padding",      "target_frame_index",
	         "timestamp",       "video_path",
	         "video_timestamp", "decoded_timestamp",
	         "width",           "height",
	         "channels",        "image"};
	return_types = {LogicalType::BIGINT,  LogicalType::BIGINT, LogicalType::BIGINT,  LogicalType::BIGINT,
	                LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::DOUBLE,  LogicalType::BIGINT,
	                LogicalType::BOOLEAN, LogicalType::BIGINT, LogicalType::DOUBLE,  LogicalType::VARCHAR,
	                LogicalType::DOUBLE,  LogicalType::DOUBLE, LogicalType::INTEGER, LogicalType::INTEGER,
	                LogicalType::INTEGER, LogicalType::BLOB};
	return make_uniq<LerobotVideoTargetsBindData>(std::move(dataset), std::move(metadata), std::move(deltas),
	                                              std::move(input_columns), options);
}

struct DecodedVideoFrame {
	idx_t target_index;
	double decoded_timestamp;
	int32_t width;
	int32_t height;
	const string *pixels;
};

#ifdef LEROBOT_HAVE_FFMPEG

string FFmpegError(int error_code) {
	char buffer[AV_ERROR_MAX_STRING_SIZE];
	if (av_strerror(error_code, buffer, sizeof(buffer)) < 0) {
		return "unknown FFmpeg error " + std::to_string(error_code);
	}
	return string(buffer);
}

struct DuckDBAVIOState {
	DuckDBAVIOState(ClientContext &context, const string &path, LerobotVideoDecodeMetrics &metrics_p)
	    : handle(FileSystem::GetFileSystem(context).OpenFile(path, FileFlags::FILE_FLAGS_READ)), position(0),
	      metrics(metrics_p) {
		auto file_size = handle->GetFileSize();
		if (file_size > static_cast<idx_t>(std::numeric_limits<int64_t>::max())) {
			throw IOException("LeRobot video file is too large for FFmpeg: '%s'", path);
		}
		size = static_cast<int64_t>(file_size);
	}

	static int Read(void *opaque, uint8_t *buffer, int buffer_size) {
		auto &state = *reinterpret_cast<DuckDBAVIOState *>(opaque);
		try {
			auto read_count = state.handle->Read(buffer, static_cast<idx_t>(buffer_size));
			if (read_count <= 0) {
				return AVERROR_EOF;
			}
			state.position += read_count;
			state.metrics.video_bytes_read.fetch_add(static_cast<uint64_t>(read_count), std::memory_order_relaxed);
			return static_cast<int>(read_count);
		} catch (std::exception &exception) {
			state.error = exception.what();
			return AVERROR(EIO);
		}
	}

	static int64_t Seek(void *opaque, int64_t offset, int whence) {
		auto &state = *reinterpret_cast<DuckDBAVIOState *>(opaque);
		whence &= ~AVSEEK_FORCE;
		if (whence == AVSEEK_SIZE) {
			return state.size;
		}
		int64_t base_position;
		if (whence == SEEK_SET) {
			base_position = 0;
		} else if (whence == SEEK_CUR) {
			base_position = state.position;
		} else if (whence == SEEK_END) {
			base_position = state.size;
		} else {
			return AVERROR(EINVAL);
		}
		if ((offset > 0 && base_position > std::numeric_limits<int64_t>::max() - offset) ||
		    (offset < 0 && base_position < std::numeric_limits<int64_t>::min() - offset)) {
			return AVERROR(EINVAL);
		}
		const auto next_position = base_position + offset;
		if (next_position < 0 || next_position > state.size) {
			return AVERROR(EINVAL);
		}
		try {
			state.handle->Seek(static_cast<idx_t>(next_position));
			state.position = next_position;
			state.metrics.avio_seeks.fetch_add(1, std::memory_order_relaxed);
			return next_position;
		} catch (std::exception &exception) {
			state.error = exception.what();
			return AVERROR(EIO);
		}
	}

	void ThrowIOError(const string &path) {
		if (!error.empty()) {
			throw IOException("Failed to read LeRobot video '%s': %s", path, error);
		}
	}

	unique_ptr<FileHandle> handle;
	int64_t size;
	int64_t position;
	string error;
	LerobotVideoDecodeMetrics &metrics;
};

class LerobotShardDecoder {
public:
	LerobotShardDecoder(ClientContext &context_p, const LerobotVideoOptions &options_p, idx_t shard_index_p,
	                    string video_path_p, const LerobotVideoFeatureMetadata &video_feature_metadata_p,
	                    bool needs_pixels_p, LerobotVideoDecodeMetrics &metrics_p)
	    : context(context_p), options(options_p), shard_index(shard_index_p), video_path(std::move(video_path_p)),
	      video_feature_metadata(video_feature_metadata_p), needs_pixels(needs_pixels_p), buffer(nullptr),
	      io_state(context_p, video_path, metrics_p), metrics(metrics_p), format_context(nullptr),
	      avio_context(nullptr), codec_context(nullptr), packet(nullptr), previous_frame(nullptr),
	      current_frame(nullptr), video_stream(nullptr), sws_context(nullptr), cluster_position(0), target_position(0),
	      decoded_frames_in_buffer(0), demux_eof(false), flush_sent(false), decoder_eof(false), have_previous(false),
	      have_current(false), previous_timestamp(0), current_timestamp(0), have_last_target(false),
	      last_target_timestamp(0), have_converted_frame(false), converted_timestamp(0), converted_source_width(0),
	      converted_source_height(0), converted_source_format(AV_PIX_FMT_NONE), depth_scale(0), depth_offset(0),
	      sws_source_width(0), sws_source_height(0), sws_source_format(AV_PIX_FMT_NONE), resize_source_width(0),
	      resize_source_height(0), resize_target_width(0), resize_target_height(0) {
		if (video_feature_metadata.is_depth_map) {
			if (video_feature_metadata.use_log) {
				const auto log_min = std::log(video_feature_metadata.depth_min + video_feature_metadata.shift);
				const auto log_max = std::log(video_feature_metadata.depth_max + video_feature_metadata.shift);
				depth_scale = (log_max - log_min) / static_cast<double>(LEROBOT_DEPTH_QMAX);
				depth_offset = log_min;
			} else {
				depth_scale = (video_feature_metadata.depth_max - video_feature_metadata.depth_min) /
				              static_cast<double>(LEROBOT_DEPTH_QMAX);
				depth_offset = video_feature_metadata.depth_min;
			}
		}
		try {
			Open();
		} catch (...) {
			Close();
			throw;
		}
	}

	~LerobotShardDecoder() {
		Close();
	}

	idx_t GetShardIndex() const {
		return shard_index;
	}

	void BeginBuffer(const LerobotDecodeBuffer &buffer_p) {
		if (buffer_p.shard_index != shard_index || buffer_p.targets.empty() || buffer_p.clusters.empty()) {
			throw InternalException("Invalid LeRobot target buffer for video shard");
		}
		buffer = &buffer_p;
		cluster_position = 0;
		target_position = 0;
		decoded_frames_in_buffer = 0;

		const auto earliest = buffer->targets[buffer->clusters.front().front()].video_timestamp;
		const bool continue_decode = have_last_target && !decoder_eof && earliest + 1e-9 >= last_target_timestamp &&
		                             earliest - last_target_timestamp <= options.cluster_gap;
		if (!continue_decode) {
			StartCluster();
		}
	}

	void Close() {
		if (sws_context) {
			sws_freeContext(sws_context);
			sws_context = nullptr;
		}
		if (previous_frame) {
			av_frame_free(&previous_frame);
		}
		if (current_frame) {
			av_frame_free(&current_frame);
		}
		if (packet) {
			av_packet_free(&packet);
		}
		if (codec_context) {
			avcodec_free_context(&codec_context);
		}
		if (format_context) {
			avformat_close_input(&format_context);
		}
		if (avio_context) {
			avio_context_free(&avio_context);
		}
	}

	bool Next(DecodedVideoFrame &result) {
		if (!buffer) {
			throw InternalException("LeRobot decoder has no target buffer");
		}
		while (cluster_position < buffer->clusters.size()) {
			auto &cluster = buffer->clusters[cluster_position];
			if (target_position >= cluster.size()) {
				cluster_position++;
				if (cluster_position >= buffer->clusters.size()) {
					return false;
				}
				StartCluster();
				continue;
			}
			CheckForInterrupt(context);
			const auto target_index = cluster[target_position];
			const auto target_timestamp = buffer->targets[target_index].video_timestamp;
			if (have_current && target_timestamp <= current_timestamp) {
				const AVFrame *selected = current_frame;
				double selected_timestamp = current_timestamp;
				if (have_previous && std::fabs(previous_timestamp - target_timestamp) <=
				                         std::fabs(current_timestamp - target_timestamp)) {
					selected = previous_frame;
					selected_timestamp = previous_timestamp;
				}
				ValidateTolerance(target_timestamp, selected_timestamp);
				ConvertFrame(*selected, selected_timestamp, target_index, result);
				target_position++;
				have_last_target = true;
				last_target_timestamp = target_timestamp;
				return true;
			}

			if (have_current) {
				av_frame_unref(previous_frame);
				av_frame_move_ref(previous_frame, current_frame);
				previous_timestamp = current_timestamp;
				have_previous = true;
				have_current = false;
			}

			if (decoder_eof || !ReadFrame()) {
				decoder_eof = true;
				if (!have_previous) {
					throw InvalidInputException("No frames decoded from LeRobot video '%s'", video_path);
				}
				ValidateTolerance(target_timestamp, previous_timestamp);
				ConvertFrame(*previous_frame, previous_timestamp, target_index, result);
				target_position++;
				have_last_target = true;
				last_target_timestamp = target_timestamp;
				return true;
			}
			have_current = true;
		}
		return false;
	}

private:
	void Open() {
		const idx_t io_buffer_size = 64 * 1024;
		auto io_buffer = reinterpret_cast<unsigned char *>(av_malloc(io_buffer_size));
		if (!io_buffer) {
			throw OutOfMemoryException("Failed to allocate FFmpeg IO buffer");
		}
		avio_context = avio_alloc_context(io_buffer, static_cast<int>(io_buffer_size), 0, &io_state,
		                                  DuckDBAVIOState::Read, nullptr, DuckDBAVIOState::Seek);
		if (!avio_context) {
			av_free(io_buffer);
			throw OutOfMemoryException("Failed to allocate FFmpeg AVIO context");
		}

		format_context = avformat_alloc_context();
		if (!format_context) {
			throw OutOfMemoryException("Failed to allocate FFmpeg format context");
		}
		format_context->pb = avio_context;
		format_context->flags |= AVFMT_FLAG_CUSTOM_IO;
		auto status = avformat_open_input(&format_context, nullptr, nullptr, nullptr);
		io_state.ThrowIOError(video_path);
		if (status < 0) {
			throw IOException("FFmpeg could not open LeRobot video '%s': %s", video_path, FFmpegError(status));
		}
		status = avformat_find_stream_info(format_context, nullptr);
		io_state.ThrowIOError(video_path);
		if (status < 0) {
			throw IOException("FFmpeg could not inspect LeRobot video '%s': %s", video_path, FFmpegError(status));
		}

		const auto stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (stream_index < 0) {
			throw InvalidInputException("LeRobot video '%s' has no decodable video stream", video_path);
		}
		video_stream = format_context->streams[stream_index];
		const AVCodec *codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
		if (!codec) {
			throw InvalidInputException("LeRobot video '%s' uses an FFmpeg codec with no available decoder",
			                            video_path);
		}
		codec_context = avcodec_alloc_context3(codec);
		if (!codec_context) {
			throw OutOfMemoryException("Failed to allocate FFmpeg codec context");
		}
		status = avcodec_parameters_to_context(codec_context, video_stream->codecpar);
		if (status < 0) {
			throw IOException("FFmpeg could not configure decoder for '%s': %s", video_path, FFmpegError(status));
		}
		codec_context->thread_count = options.codec_threads;
		status = avcodec_open2(codec_context, codec, nullptr);
		if (status < 0) {
			throw IOException("FFmpeg could not start decoder for '%s': %s", video_path, FFmpegError(status));
		}

		packet = av_packet_alloc();
		previous_frame = av_frame_alloc();
		current_frame = av_frame_alloc();
		if (!packet || !previous_frame || !current_frame) {
			throw OutOfMemoryException("Failed to allocate FFmpeg decode frames");
		}
		metrics.decoder_opens.fetch_add(1, std::memory_order_relaxed);
	}

	void StartCluster() {
		av_packet_unref(packet);
		av_frame_unref(previous_frame);
		av_frame_unref(current_frame);
		target_position = 0;
		demux_eof = false;
		flush_sent = false;
		decoder_eof = false;
		have_previous = false;
		have_current = false;
		have_converted_frame = false;

		const auto &cluster = buffer->clusters[cluster_position];
		const auto earliest = buffer->targets[cluster.front()].video_timestamp;
		const auto stream_time_base = av_q2d(video_stream->time_base);
		if (!std::isfinite(stream_time_base) || stream_time_base <= 0 ||
		    earliest > static_cast<double>(std::numeric_limits<int64_t>::max()) * stream_time_base) {
			throw InvalidInputException("LeRobot video timestamp %.6f is too large to seek in '%s'", earliest,
			                            video_path);
		}
		auto seek_timestamp = static_cast<int64_t>(std::llround(std::max(0.0, earliest) / stream_time_base));
		if (seek_timestamp > 0) {
			seek_timestamp--;
		}
		auto status = av_seek_frame(format_context, video_stream->index, seek_timestamp, AVSEEK_FLAG_BACKWARD);
		metrics.decoder_seeks.fetch_add(1, std::memory_order_relaxed);
		io_state.ThrowIOError(video_path);
		if (status < 0) {
			throw IOException("FFmpeg could not seek LeRobot video '%s' to %.6f seconds: %s", video_path, earliest,
			                  FFmpegError(status));
		}
		avcodec_flush_buffers(codec_context);
	}

	bool ReadFrame() {
		av_frame_unref(current_frame);
		while (true) {
			auto status = avcodec_receive_frame(codec_context, current_frame);
			if (status == 0) {
				int64_t timestamp = current_frame->pts;
				if (timestamp == AV_NOPTS_VALUE) {
					timestamp = current_frame->best_effort_timestamp;
				}
				if (timestamp == AV_NOPTS_VALUE) {
					av_frame_unref(current_frame);
					continue;
				}
				current_timestamp = static_cast<double>(timestamp) * av_q2d(video_stream->time_base);
				if (!std::isfinite(current_timestamp)) {
					av_frame_unref(current_frame);
					continue;
				}
				if (have_previous && current_timestamp + 1e-9 < previous_timestamp) {
					throw InvalidInputException("FFmpeg returned non-monotonic timestamps for LeRobot video '%s'",
					                            video_path);
				}
				decoded_frames_in_buffer++;
				metrics.frames_decoded.fetch_add(1, std::memory_order_relaxed);
				if ((decoded_frames_in_buffer & 255) == 0) {
					CheckForInterrupt(context);
				}
				if (decoded_frames_in_buffer > LEROBOT_DECODE_FRAME_BUDGET) {
					throw InvalidInputException("Exceeded the %d-frame decode budget while aligning LeRobot video '%s'",
					                            LEROBOT_DECODE_FRAME_BUDGET, video_path);
				}
				return true;
			}
			if (status == AVERROR_EOF) {
				return false;
			}
			if (status != AVERROR(EAGAIN)) {
				throw IOException("FFmpeg failed while decoding LeRobot video '%s': %s", video_path,
				                  FFmpegError(status));
			}

			if (demux_eof) {
				if (!flush_sent) {
					status = avcodec_send_packet(codec_context, nullptr);
					flush_sent = true;
					if (status < 0 && status != AVERROR_EOF) {
						throw IOException("FFmpeg failed to flush LeRobot video '%s': %s", video_path,
						                  FFmpegError(status));
					}
					continue;
				}
				return false;
			}

			while (true) {
				status = av_read_frame(format_context, packet);
				io_state.ThrowIOError(video_path);
				if (status < 0) {
					if (status != AVERROR_EOF) {
						throw IOException("FFmpeg failed while reading LeRobot video '%s': %s", video_path,
						                  FFmpegError(status));
					}
					demux_eof = true;
					break;
				}
				if (packet->stream_index != video_stream->index) {
					av_packet_unref(packet);
					continue;
				}
				status = avcodec_send_packet(codec_context, packet);
				av_packet_unref(packet);
				if (status < 0) {
					throw IOException("FFmpeg failed to submit a packet for LeRobot video '%s': %s", video_path,
					                  FFmpegError(status));
				}
				break;
			}
		}
	}

	void ValidateTolerance(double target_timestamp, double decoded_timestamp) const {
		const auto distance = std::fabs(target_timestamp - decoded_timestamp);
		if (distance > options.tolerance) {
			throw InvalidInputException("No frame in LeRobot video '%s' matched timestamp %.6f within tolerance %.6f "
			                            "(closest decoded timestamp %.6f, distance %.6f)",
			                            video_path, target_timestamp, options.tolerance, decoded_timestamp, distance);
		}
	}

	void PrepareResizeIndices(int32_t source_width, int32_t source_height, int32_t target_width,
	                          int32_t target_height) {
		if (resize_source_width == source_width && resize_source_height == source_height &&
		    resize_target_width == target_width && resize_target_height == target_height) {
			return;
		}
		resize_x_indices.resize(target_width);
		resize_y_indices.resize(target_height);
		const auto horizontal_scale =
		    static_cast<double>(static_cast<float>(source_width)) / static_cast<double>(target_width);
		const auto vertical_scale =
		    static_cast<double>(static_cast<float>(source_height)) / static_cast<double>(target_height);
		double source_x = horizontal_scale * 0.5;
		for (int32_t target_x = 0; target_x < target_width; target_x++) {
			resize_x_indices[target_x] = std::min<int32_t>(source_width - 1, static_cast<int32_t>(source_x));
			source_x += horizontal_scale;
		}
		double source_y = vertical_scale * 0.5;
		for (int32_t target_y = 0; target_y < target_height; target_y++) {
			resize_y_indices[target_y] = std::min<int32_t>(source_height - 1, static_cast<int32_t>(source_y));
			source_y += vertical_scale;
		}
		resize_source_width = source_width;
		resize_source_height = source_height;
		resize_target_width = target_width;
		resize_target_height = target_height;
	}

	float DequantizeDepth(uint16_t quantized) const {
		float depth = static_cast<float>(quantized) * depth_scale;
		depth += depth_offset;
		if (video_feature_metadata.use_log) {
			depth = std::exp(depth);
			depth -= static_cast<float>(video_feature_metadata.shift);
		}
		depth = MaxValue(static_cast<float>(video_feature_metadata.depth_min),
		                 MinValue(static_cast<float>(video_feature_metadata.depth_max), depth));
		if (options.depth_output_unit == LerobotDepthOutputUnit::MILLIMETERS) {
			depth *= 1000.0f;
			if (depth <= 0.0f) {
				return 0.0f;
			}
			if (depth >= 65535.0f) {
				return 65535.0f;
			}
			const auto lower = std::floor(depth);
			const auto fraction = depth - lower;
			const auto lower_is_odd = (static_cast<uint32_t>(lower) & 1U) != 0;
			if (fraction > 0.5f || (fraction == 0.5f && lower_is_odd)) {
				return lower + 1.0f;
			}
			return lower;
		}
		return depth;
	}

	void ConvertDepthPixels(const AVFrame &source, int32_t target_width, int32_t target_height) {
		const auto source_format = static_cast<AVPixelFormat>(source.format);
		if (source_format != AV_PIX_FMT_GRAY12LE) {
			throw InvalidInputException("LeRobot depth video '%s' decoded as pixel format %d instead of gray12le",
			                            video_path, static_cast<int>(source_format));
		}
		if (!source.data[0] || static_cast<int64_t>(source.linesize[0]) <
		                           static_cast<int64_t>(source.width) * static_cast<int64_t>(sizeof(uint16_t))) {
			throw InvalidInputException("FFmpeg returned an invalid gray12le plane for LeRobot depth video '%s'",
			                            video_path);
		}
		const uint64_t target_byte_count =
		    static_cast<uint64_t>(target_width) * static_cast<uint64_t>(target_height) * sizeof(float);
		if (target_byte_count > static_cast<uint64_t>(std::numeric_limits<idx_t>::max())) {
			throw OutOfMemoryException("Decoded LeRobot depth frame is too large");
		}
		const bool resize = target_width != source.width || target_height != source.height;
		if (resize) {
			PrepareResizeIndices(source.width, source.height, target_width, target_height);
		}
		converted_pixels.resize(static_cast<idx_t>(target_byte_count));
		for (int32_t target_y = 0; target_y < target_height; target_y++) {
			const auto source_y = resize ? resize_y_indices[target_y] : target_y;
			const auto source_row = source.data[0] + static_cast<idx_t>(source_y) * source.linesize[0];
			for (int32_t target_x = 0; target_x < target_width; target_x++) {
				const auto source_x = resize ? resize_x_indices[target_x] : target_x;
				const auto quantized = LoadLE<uint16_t>(source_row + static_cast<idx_t>(source_x) * sizeof(uint16_t));
				if (quantized > LEROBOT_DEPTH_QMAX) {
					throw InvalidInputException("LeRobot depth video '%s' contains code %u above the 12-bit range",
					                            video_path, static_cast<unsigned>(quantized));
				}
				const auto depth = BSwapIfBE(DequantizeDepth(quantized));
				const auto target_offset =
				    (static_cast<idx_t>(target_y) * static_cast<idx_t>(target_width) + target_x) * sizeof(float);
				Store<float>(depth, reinterpret_cast<data_ptr_t>(&converted_pixels[target_offset]));
			}
		}
	}

	void ConvertRGBPixels(const AVFrame &source, int32_t target_width, int32_t target_height) {
		const uint64_t source_byte_count =
		    static_cast<uint64_t>(source.width) * static_cast<uint64_t>(source.height) * 3;
		const uint64_t target_byte_count =
		    static_cast<uint64_t>(target_width) * static_cast<uint64_t>(target_height) * 3;
		if (source_byte_count > static_cast<uint64_t>(std::numeric_limits<idx_t>::max()) ||
		    target_byte_count > static_cast<uint64_t>(std::numeric_limits<idx_t>::max())) {
			throw OutOfMemoryException("Decoded LeRobot RGB frame is too large");
		}

		const auto source_format = static_cast<AVPixelFormat>(source.format);
		if (!sws_context || sws_source_width != source.width || sws_source_height != source.height ||
		    sws_source_format != source_format) {
			if (sws_context) {
				sws_freeContext(sws_context);
			}
			// PyAV first converts to RGB24 at the native dimensions. Daft then applies
			// Pillow's nearest-neighbour resize as a separate operation.
			sws_context = sws_getContext(source.width, source.height, source_format, source.width, source.height,
			                             AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
			if (!sws_context) {
				throw InvalidInputException("FFmpeg cannot convert LeRobot video '%s' from pixel format %d to RGB24",
				                            video_path, static_cast<int>(source_format));
			}
			sws_source_width = source.width;
			sws_source_height = source.height;
			sws_source_format = source_format;
		}

		const bool resize = target_width != source.width || target_height != source.height;
		string &rgb_pixels = resize ? source_rgb_pixels : converted_pixels;
		rgb_pixels.resize(static_cast<idx_t>(source_byte_count));
		uint8_t *destination_data[4] = {reinterpret_cast<uint8_t *>(&rgb_pixels[0]), nullptr, nullptr, nullptr};
		int destination_linesize[4] = {source.width * 3, 0, 0, 0};
		const auto scaled_height = sws_scale(sws_context, source.data, source.linesize, 0, source.height,
		                                     destination_data, destination_linesize);
		if (scaled_height != source.height) {
			throw IOException("FFmpeg converted only %d of %d rows for LeRobot video '%s'", scaled_height,
			                  source.height, video_path);
		}

		if (resize) {
			PrepareResizeIndices(source.width, source.height, target_width, target_height);
			converted_pixels.resize(static_cast<idx_t>(target_byte_count));
			for (int32_t target_y = 0; target_y < target_height; target_y++) {
				const auto source_y = resize_y_indices[target_y];
				for (int32_t target_x = 0; target_x < target_width; target_x++) {
					const auto source_x = resize_x_indices[target_x];
					const auto source_offset =
					    (static_cast<idx_t>(source_y) * static_cast<idx_t>(source.width) + source_x) * 3;
					const auto target_offset =
					    (static_cast<idx_t>(target_y) * static_cast<idx_t>(target_width) + target_x) * 3;
					std::memcpy(&converted_pixels[target_offset], &source_rgb_pixels[source_offset], 3);
				}
			}
		}
	}

	void ConvertFrame(const AVFrame &source, double decoded_timestamp, idx_t target_index, DecodedVideoFrame &result) {
		const auto target_width = options.width > 0 ? options.width : source.width;
		const auto target_height = options.height > 0 ? options.height : source.height;
		if (source.width <= 0 || source.height <= 0 || target_width <= 0 || target_height <= 0) {
			throw InvalidInputException("FFmpeg returned invalid dimensions for LeRobot video '%s'", video_path);
		}
		result.target_index = target_index;
		result.decoded_timestamp = decoded_timestamp;
		result.width = target_width;
		result.height = target_height;
		result.pixels = nullptr;
		const auto source_format = static_cast<AVPixelFormat>(source.format);
		if (video_feature_metadata.is_depth_map && source_format != AV_PIX_FMT_GRAY12LE) {
			throw InvalidInputException("LeRobot depth video '%s' decoded as pixel format %d instead of gray12le",
			                            video_path, static_cast<int>(source_format));
		}
		if (!needs_pixels) {
			return;
		}

		if (have_converted_frame && decoded_timestamp == converted_timestamp &&
		    source.width == converted_source_width && source.height == converted_source_height &&
		    source_format == converted_source_format) {
			if (video_feature_metadata.is_depth_map) {
				metrics.depth_fanout_hits.fetch_add(1, std::memory_order_relaxed);
			} else {
				metrics.rgb_fanout_hits.fetch_add(1, std::memory_order_relaxed);
			}
			result.pixels = &converted_pixels;
			return;
		}
		if (video_feature_metadata.is_depth_map) {
			metrics.depth_conversions.fetch_add(1, std::memory_order_relaxed);
			ConvertDepthPixels(source, target_width, target_height);
		} else {
			metrics.rgb_conversions.fetch_add(1, std::memory_order_relaxed);
			ConvertRGBPixels(source, target_width, target_height);
		}
		have_converted_frame = true;
		converted_timestamp = decoded_timestamp;
		converted_source_width = source.width;
		converted_source_height = source.height;
		converted_source_format = source_format;
		result.pixels = &converted_pixels;
	}

	ClientContext &context;
	LerobotVideoOptions options;
	idx_t shard_index;
	string video_path;
	LerobotVideoFeatureMetadata video_feature_metadata;
	bool needs_pixels;
	const LerobotDecodeBuffer *buffer;
	DuckDBAVIOState io_state;
	LerobotVideoDecodeMetrics &metrics;
	AVFormatContext *format_context;
	AVIOContext *avio_context;
	AVCodecContext *codec_context;
	AVPacket *packet;
	AVFrame *previous_frame;
	AVFrame *current_frame;
	AVStream *video_stream;
	SwsContext *sws_context;
	idx_t cluster_position;
	idx_t target_position;
	idx_t decoded_frames_in_buffer;
	bool demux_eof;
	bool flush_sent;
	bool decoder_eof;
	bool have_previous;
	bool have_current;
	double previous_timestamp;
	double current_timestamp;
	bool have_last_target;
	double last_target_timestamp;
	bool have_converted_frame;
	double converted_timestamp;
	int converted_source_width;
	int converted_source_height;
	AVPixelFormat converted_source_format;
	float depth_scale;
	float depth_offset;
	string source_rgb_pixels;
	string converted_pixels;
	int sws_source_width;
	int sws_source_height;
	AVPixelFormat sws_source_format;
	vector<int32_t> resize_x_indices;
	vector<int32_t> resize_y_indices;
	int resize_source_width;
	int resize_source_height;
	int resize_target_width;
	int resize_target_height;
};

class LerobotDecoderCache {
public:
	LerobotDecoderCache(idx_t max_cached_decoders_p, LerobotVideoDecodeMetrics &metrics_p)
	    : max_cached_decoders(max_cached_decoders_p), open_count(0), metrics(metrics_p) {
	}

	unique_ptr<LerobotShardDecoder> Acquire(ClientContext &context, const LerobotVideoOptions &options,
	                                        idx_t shard_index, const string &video_path,
	                                        const LerobotVideoFeatureMetadata &video_feature_metadata,
	                                        bool needs_pixels) {
		metrics.decoder_acquires.fetch_add(1, std::memory_order_relaxed);
		unique_ptr<LerobotShardDecoder> stale_decoder;
		{
			unique_lock<mutex> guard(lock);
			while (true) {
				auto entry = idle_decoders.find(shard_index);
				if (entry != idle_decoders.end() && !entry->second.empty()) {
					auto result = std::move(entry->second.back());
					entry->second.pop_back();
					if (entry->second.empty()) {
						idle_decoders.erase(entry);
					}
					auto lru_entry = std::find(idle_lru.begin(), idle_lru.end(), shard_index);
					D_ASSERT(lru_entry != idle_lru.end());
					idle_lru.erase(lru_entry);
					metrics.decoder_cache_hits.fetch_add(1, std::memory_order_relaxed);
					return result;
				}

				if (open_count < max_cached_decoders) {
					open_count++;
					break;
				}
				if (idle_lru.empty()) {
					state_changed.wait(guard);
					continue;
				}
				const auto stale_shard = idle_lru.back();
				idle_lru.pop_back();
				auto stale_entry = idle_decoders.find(stale_shard);
				D_ASSERT(stale_entry != idle_decoders.end() && !stale_entry->second.empty());
				stale_decoder = std::move(stale_entry->second.front());
				stale_entry->second.pop_front();
				if (stale_entry->second.empty()) {
					idle_decoders.erase(stale_entry);
				}
				metrics.decoder_evictions.fetch_add(1, std::memory_order_relaxed);
				break;
			}
		}

		// Closing an evicted remote handle and opening its replacement can perform
		// filesystem work, so neither operation holds the cache mutex.
		stale_decoder.reset();
		try {
			return make_uniq<LerobotShardDecoder>(context, options, shard_index, video_path, video_feature_metadata,
			                                      needs_pixels, metrics);
		} catch (...) {
			lock_guard<mutex> guard(lock);
			open_count--;
			state_changed.notify_all();
			throw;
		}
	}

	void Release(idx_t shard_index, unique_ptr<LerobotShardDecoder> decoder) {
		if (!decoder) {
			return;
		}
		D_ASSERT(decoder->GetShardIndex() == shard_index);
		lock_guard<mutex> guard(lock);
		idle_lru.push_front(shard_index);
		idle_decoders[shard_index].push_back(std::move(decoder));
		state_changed.notify_all();
	}

	void Discard(unique_ptr<LerobotShardDecoder> decoder) {
		if (!decoder) {
			return;
		}
		{
			lock_guard<mutex> guard(lock);
			D_ASSERT(open_count > 0);
			open_count--;
			state_changed.notify_all();
		}
		decoder.reset();
	}

private:
	mutex lock;
	std::condition_variable state_changed;
	idx_t max_cached_decoders;
	idx_t open_count;
	list<idx_t> idle_lru;
	unordered_map<idx_t, deque<unique_ptr<LerobotShardDecoder>>> idle_decoders;
	LerobotVideoDecodeMetrics &metrics;
};

#endif

string LerobotFrameKey(int64_t episode_index, int64_t frame_index) {
	return std::to_string(episode_index) + ":" + std::to_string(frame_index);
}

template <class T>
T ReadUnifiedValue(const vector<UnifiedVectorFormat> &formats, idx_t column, idx_t row, const char *column_name) {
	const auto &format = formats[column];
	const auto source_index = format.sel->get_index(row);
	if (!format.validity.RowIsValid(source_index)) {
		throw InvalidInputException("lerobot_video_targets input column '%s' must not contain NULL", column_name);
	}
	return format.GetData<T>()[source_index];
}

int64_t ReadUnifiedInteger(const vector<UnifiedVectorFormat> &formats, idx_t column, idx_t row,
                           const char *column_name) {
	const auto &format = formats[column];
	const auto source_index = format.sel->get_index(row);
	if (!format.validity.RowIsValid(source_index)) {
		throw InvalidInputException("lerobot_video_targets input column '%s' must not contain NULL", column_name);
	}
	switch (format.physical_type) {
	case PhysicalType::INT8:
		return format.GetData<int8_t>()[source_index];
	case PhysicalType::INT16:
		return format.GetData<int16_t>()[source_index];
	case PhysicalType::INT32:
		return format.GetData<int32_t>()[source_index];
	case PhysicalType::INT64:
		return format.GetData<int64_t>()[source_index];
	case PhysicalType::UINT8:
		return format.GetData<uint8_t>()[source_index];
	case PhysicalType::UINT16:
		return format.GetData<uint16_t>()[source_index];
	case PhysicalType::UINT32:
		return format.GetData<uint32_t>()[source_index];
	case PhysicalType::UINT64: {
		const auto value = format.GetData<uint64_t>()[source_index];
		if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			throw InvalidInputException("lerobot_video_targets input column '%s' is too large", column_name);
		}
		return static_cast<int64_t>(value);
	}
	default:
		throw InternalException("Expected integral vector for lerobot_video_targets column '%s'", column_name);
	}
}

struct LerobotVideoTargetsGlobalState final : public GlobalTableFunctionState {
	LerobotVideoTargetsGlobalState(const LerobotVideoTargetsBindData &bind_data_p, const vector<column_t> &column_ids)
	    : bind_data(bind_data_p), next_target_ordinal(0), needs_decode(false), needs_pixels(false) {
		for (const auto column_id : column_ids) {
			const auto logical_column = static_cast<idx_t>(column_id);
			if (logical_column >= LEROBOT_TARGET_COLUMN_COUNT) {
				throw InternalException("Invalid projected column for lerobot_video_targets");
			}
			projected_columns.push_back(logical_column);
		}
		needs_pixels = IsProjected(LEROBOT_TARGET_IMAGE);
		needs_decode = needs_pixels || IsProjected(LEROBOT_TARGET_DECODED_TIMESTAMP) ||
		               ((bind_data.options.width == 0 || bind_data.options.height == 0) &&
		                (IsProjected(LEROBOT_TARGET_WIDTH) || IsProjected(LEROBOT_TARGET_HEIGHT)));
#ifdef LEROBOT_HAVE_FFMPEG
		if (needs_decode) {
			decoder_cache = make_uniq<LerobotDecoderCache>(bind_data.options.max_cached_decoders, metrics);
		}
#endif
	}

	idx_t MaxThreads() const override {
		return needs_decode ? std::min<idx_t>(bind_data.options.decode_threads, bind_data.options.max_cached_decoders)
		                    : bind_data.options.decode_threads;
	}

	idx_t ClaimTargetOrdinals(idx_t count) {
		return static_cast<idx_t>(
		    next_target_ordinal.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed));
	}

	const vector<idx_t> &GetProjectedColumns() const {
		return projected_columns;
	}

	bool NeedsDecode() const {
		return needs_decode;
	}

	bool NeedsPixels() const {
		return needs_pixels;
	}

	LerobotVideoDecodeMetrics &GetMetrics() {
		return metrics;
	}

#ifdef LEROBOT_HAVE_FFMPEG
	unique_ptr<LerobotShardDecoder> AcquireDecoder(ClientContext &context, const LerobotVideoRoute &route) {
		return decoder_cache->Acquire(context, bind_data.options, route.video_file_index,
		                              bind_data.metadata->GetVideoFile(route),
		                              bind_data.metadata->GetVideoFeatureMetadata(route), needs_pixels);
	}

	void ReleaseDecoder(idx_t shard_index, unique_ptr<LerobotShardDecoder> decoder) {
		decoder_cache->Release(shard_index, std::move(decoder));
	}

	void DiscardDecoder(unique_ptr<LerobotShardDecoder> decoder) {
		decoder_cache->Discard(std::move(decoder));
	}
#endif

private:
	bool IsProjected(idx_t logical_column) const {
		return std::find(projected_columns.begin(), projected_columns.end(), logical_column) != projected_columns.end();
	}

	const LerobotVideoTargetsBindData &bind_data;
	std::atomic<uint64_t> next_target_ordinal;
	vector<idx_t> projected_columns;
	bool needs_decode;
	bool needs_pixels;
	LerobotVideoDecodeMetrics metrics;
#ifdef LEROBOT_HAVE_FFMPEG
	unique_ptr<LerobotDecoderCache> decoder_cache;
#endif
};

struct LerobotVideoTargetsLocalState final : public LocalTableFunctionState {
	LerobotVideoTargetsLocalState()
	    : input_initialized(false), input_position(0), target_position(0), have_decoded(false) {
	}

	void InitializeInput(DataChunk &input) {
		input_formats.clear();
		input_formats.reserve(input.ColumnCount());
		for (idx_t column = 0; column < input.ColumnCount(); column++) {
			input_formats.push_back(UnifiedVectorFormat());
			PrepareUnifiedFormat(input.data[column], input.size(), input_formats.back());
		}
		input_position = 0;
		input_initialized = true;
	}

	bool InputFinished(const DataChunk &input) const {
		return input_initialized && input_position >= input.size() && buffers.empty() && !buffer;
	}

	void FinishInput() {
		input_initialized = false;
		input_position = 0;
		input_formats.clear();
		routes.clear();
		buffers.clear();
		buffer.reset();
	}

	vector<UnifiedVectorFormat> input_formats;
	bool input_initialized;
	idx_t input_position;
	vector<LerobotVideoRoute> routes;
	deque<unique_ptr<LerobotDecodeBuffer>> buffers;
	unique_ptr<LerobotDecodeBuffer> buffer;
	idx_t target_position;
#ifdef LEROBOT_HAVE_FFMPEG
	unique_ptr<LerobotShardDecoder> decoder;
#endif
	DecodedVideoFrame decoded;
	bool have_decoded;
};

string BuildTargetTimestampQuery(const vector<std::pair<int64_t, int64_t>> &frame_keys,
                                 const vector<string> &data_files) {
	string values;
	for (idx_t index = 0; index < frame_keys.size(); index++) {
		if (!values.empty()) {
			values += ", ";
		}
		values += "(" + std::to_string(frame_keys[index].first) + ", " + std::to_string(frame_keys[index].second) + ")";
	}
	return "WITH requested(episode_index, frame_index) AS (VALUES " + values +
	       "), frames AS (SELECT CAST(episode_index AS BIGINT) AS episode_index, "
	       "CAST(frame_index AS BIGINT) AS frame_index, CAST(timestamp AS DOUBLE) AS timestamp "
	       "FROM read_parquet(" +
	       ValueListSQL(data_files) +
	       ")) SELECT requested.episode_index, requested.frame_index, min(frames.timestamp), "
	       "CAST(count(frames.frame_index) AS BIGINT) FROM requested LEFT JOIN frames "
	       "ON frames.episode_index = requested.episode_index AND frames.frame_index = requested.frame_index "
	       "GROUP BY requested.episode_index, requested.frame_index";
}

unordered_map<string, double> ReadTargetTimestamps(ClientContext &context, const LerobotVideoTargetsBindData &bind_data,
                                                   const vector<LerobotDecodeTarget> &targets) {
	vector<std::pair<int64_t, int64_t>> frame_keys;
	vector<int64_t> episode_indices;
	frame_keys.reserve(targets.size());
	episode_indices.reserve(targets.size());
	for (const auto &target : targets) {
		frame_keys.push_back(std::make_pair(target.episode_index, target.target_frame_index));
		episode_indices.push_back(target.episode_index);
	}
	std::sort(frame_keys.begin(), frame_keys.end());
	frame_keys.erase(std::unique(frame_keys.begin(), frame_keys.end()), frame_keys.end());
	std::sort(episode_indices.begin(), episode_indices.end());
	episode_indices.erase(std::unique(episode_indices.begin(), episode_indices.end()), episode_indices.end());
	auto data_files = bind_data.dataset->ResolveDataFiles(episode_indices);
	if (data_files.empty()) {
		throw InvalidInputException("LeRobot target episodes do not resolve to a Parquet data shard");
	}

	Connection connection(*context.db);
	auto result = connection.SendQuery(BuildTargetTimestampQuery(frame_keys, data_files));
	if (result->HasError()) {
		throw InvalidInputException("Failed to read LeRobot target timestamps: %s", result->GetError());
	}
	unordered_map<string, double> timestamps;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk) {
			break;
		}
		vector<UnifiedVectorFormat> formats;
		formats.reserve(chunk->ColumnCount());
		for (idx_t column = 0; column < chunk->ColumnCount(); column++) {
			formats.push_back(UnifiedVectorFormat());
			PrepareUnifiedFormat(chunk->data[column], chunk->size(), formats.back());
		}
		for (idx_t row = 0; row < chunk->size(); row++) {
			const auto episode_index = ReadUnifiedInteger(formats, 0, row, "episode_index");
			const auto frame_index = ReadUnifiedInteger(formats, 1, row, "frame_index");
			const auto match_count = ReadUnifiedInteger(formats, 3, row, "match_count");
			const auto timestamp_index = formats[2].sel->get_index(row);
			if (match_count != 1 || !formats[2].validity.RowIsValid(timestamp_index)) {
				if (match_count == 0) {
					throw InvalidInputException("LeRobot episode %d has no Parquet row for frame %d", episode_index,
					                            frame_index);
				}
				throw InvalidInputException("LeRobot episode %d has %d Parquet rows for frame %d", episode_index,
				                            match_count, frame_index);
			}
			const auto timestamp = formats[2].GetData<double>()[timestamp_index];
			if (!std::isfinite(timestamp) || timestamp < 0) {
				throw InvalidInputException("Invalid LeRobot timestamp for episode %d, frame %d", episode_index,
				                            frame_index);
			}
			timestamps.emplace(LerobotFrameKey(episode_index, frame_index), timestamp);
		}
	}
	if (result->HasError()) {
		throw InvalidInputException("Failed to read LeRobot target timestamps: %s", result->GetError());
	}
	return timestamps;
}

void BuildTargetBuffers(ClientContext &context, const LerobotVideoTargetsBindData &bind_data,
                        LerobotVideoTargetsGlobalState &global_state, LerobotVideoTargetsLocalState &local_state,
                        DataChunk &input) {
	const auto remaining = input.size() - local_state.input_position;
	const auto batch_count = std::min<idx_t>(remaining, bind_data.options.max_pending_targets);
	if (batch_count == 0) {
		return;
	}
	local_state.routes.clear();
	vector<LerobotDecodeTarget> targets;
	targets.reserve(batch_count);
	local_state.routes.reserve(batch_count);
	const auto first_ordinal = global_state.ClaimTargetOrdinals(batch_count);
	const char *input_names[] = {"request_id", "episode_index", "frame_index", "video_key", "delta_index"};

	for (idx_t offset = 0; offset < batch_count; offset++) {
		const auto row = local_state.input_position + offset;
		const auto request_id =
		    ReadUnifiedInteger(local_state.input_formats, bind_data.input_columns[0], row, input_names[0]);
		const auto episode_index =
		    ReadUnifiedInteger(local_state.input_formats, bind_data.input_columns[1], row, input_names[1]);
		const auto frame_index =
		    ReadUnifiedInteger(local_state.input_formats, bind_data.input_columns[2], row, input_names[2]);
		const auto video_key_value =
		    ReadUnifiedValue<string_t>(local_state.input_formats, bind_data.input_columns[3], row, input_names[3]);
		const auto delta_index =
		    ReadUnifiedInteger(local_state.input_formats, bind_data.input_columns[4], row, input_names[4]);
		if (episode_index < 0 || frame_index < 0) {
			throw InvalidInputException("lerobot_video_targets episode_index and frame_index must be non-negative");
		}
		if (delta_index < 0 || static_cast<uint64_t>(delta_index) >= bind_data.deltas.size()) {
			throw InvalidInputException("lerobot_video_targets delta_index %d is outside delta_timestamps length %d",
			                            delta_index, bind_data.deltas.size());
		}
		const auto video_key = video_key_value.GetString();
		if (video_key.empty()) {
			throw InvalidInputException("lerobot_video_targets video_key must not be empty");
		}
		const auto route = bind_data.metadata->FindRoute(episode_index, video_key);
		if (!route) {
			throw InvalidInputException("LeRobot episode %d has no video route for key '%s'", episode_index, video_key);
		}
		if (frame_index >= route->episode_length) {
			throw InvalidInputException("LeRobot frame %d is outside episode %d length %d", frame_index, episode_index,
			                            route->episode_length);
		}
		const auto &delta = bind_data.deltas[static_cast<idx_t>(delta_index)];
		const auto last_frame_index = route->episode_length - 1;
		int64_t target_frame_index;
		bool is_padding = false;
		if (delta.frame_offset < -frame_index) {
			target_frame_index = 0;
			is_padding = true;
		} else if (delta.frame_offset > last_frame_index - frame_index) {
			target_frame_index = last_frame_index;
			is_padding = true;
		} else {
			target_frame_index = frame_index + delta.frame_offset;
		}
		const auto route_index = local_state.routes.size();
		local_state.routes.push_back(*route);
		targets.push_back(LerobotDecodeTarget(request_id, first_ordinal + offset, static_cast<idx_t>(delta_index),
		                                      delta.timestamp, delta.frame_offset, is_padding, episode_index,
		                                      frame_index, target_frame_index, 0, 0, route_index));
	}
	local_state.input_position += batch_count;
	auto timestamps = ReadTargetTimestamps(context, bind_data, targets);
	unordered_map<idx_t, vector<LerobotDecodeTarget>> targets_by_shard;
	for (auto &target : targets) {
		auto timestamp_entry = timestamps.find(LerobotFrameKey(target.episode_index, target.target_frame_index));
		if (timestamp_entry == timestamps.end()) {
			throw InternalException("Missing resolved LeRobot target timestamp");
		}
		target.frame_timestamp = timestamp_entry->second;
		const auto &route = local_state.routes[target.route_index];
		target.video_timestamp = route.from_timestamp + target.frame_timestamp;
		if (!std::isfinite(target.video_timestamp) ||
		    target.video_timestamp < route.from_timestamp - bind_data.options.tolerance ||
		    target.video_timestamp > route.to_timestamp + bind_data.options.tolerance) {
			throw InvalidInputException("LeRobot episode %d frame %d timestamp %.6f falls outside video route "
			                            "[%.6f, %.6f] for key '%s'",
			                            target.episode_index, target.target_frame_index, target.video_timestamp,
			                            route.from_timestamp, route.to_timestamp,
			                            bind_data.metadata->GetVideoKey(route));
		}
		targets_by_shard[route.video_file_index].push_back(target);
	}
	global_state.GetMetrics().targets.fetch_add(static_cast<uint64_t>(targets.size()), std::memory_order_relaxed);
	for (auto &shard_targets : targets_by_shard) {
		idx_t position = 0;
		while (position < shard_targets.second.size()) {
			auto buffer = make_uniq<LerobotDecodeBuffer>();
			buffer->shard_index = shard_targets.first;
			const auto count =
			    std::min<idx_t>(bind_data.options.target_buffer_size, shard_targets.second.size() - position);
			buffer->targets.insert(buffer->targets.end(), shard_targets.second.begin() + position,
			                       shard_targets.second.begin() + position + count);
			FinalizeDecodeBuffer(*buffer, bind_data.options.cluster_gap);
			local_state.buffers.push_back(std::move(buffer));
			position += count;
		}
	}
}

unique_ptr<GlobalTableFunctionState> LerobotVideoTargetsInitGlobal(ClientContext &, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoTargetsBindData>();
	return make_uniq<LerobotVideoTargetsGlobalState>(bind_data, input.column_ids);
}

unique_ptr<LocalTableFunctionState> LerobotVideoTargetsInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                                 GlobalTableFunctionState *) {
	return make_uniq<LerobotVideoTargetsLocalState>();
}

struct LerobotPartialBuffer {
	vector<LerobotDecodeTarget> targets;
};

struct LerobotVideoFramesGlobalState final : public GlobalTableFunctionState {
	LerobotVideoFramesGlobalState(ClientContext &context, const LerobotVideoFramesBindData &bind_data_p,
	                              const vector<column_t> &column_ids)
	    : bind_data(bind_data_p), source_exhausted(bind_data.frame_query.empty()), producer_active(false),
	      current_row(0), pending_target_count(0), max_threads(1), needs_decode(false), needs_pixels(false) {
		const auto output_column_count = bind_data.window_mode ? static_cast<idx_t>(LEROBOT_WINDOW_COLUMN_COUNT)
		                                                       : static_cast<idx_t>(LEROBOT_FRAME_COLUMN_COUNT);
		for (const auto column_id : column_ids) {
			const auto logical_column = static_cast<idx_t>(column_id);
			if (logical_column >= output_column_count) {
				throw InternalException("Invalid projected column for LeRobot video table function");
			}
			projected_columns.push_back(logical_column);
		}
		const auto decoded_timestamp_column = bind_data.window_mode
		                                          ? static_cast<idx_t>(LEROBOT_WINDOW_DECODED_TIMESTAMP)
		                                          : static_cast<idx_t>(LEROBOT_FRAME_DECODED_TIMESTAMP);
		const auto width_column =
		    bind_data.window_mode ? static_cast<idx_t>(LEROBOT_WINDOW_WIDTH) : static_cast<idx_t>(LEROBOT_FRAME_WIDTH);
		const auto height_column = bind_data.window_mode ? static_cast<idx_t>(LEROBOT_WINDOW_HEIGHT)
		                                                 : static_cast<idx_t>(LEROBOT_FRAME_HEIGHT);
		const auto image_column =
		    bind_data.window_mode ? static_cast<idx_t>(LEROBOT_WINDOW_IMAGE) : static_cast<idx_t>(LEROBOT_FRAME_IMAGE);
		needs_pixels = IsProjected(image_column);
		needs_decode = needs_pixels || IsProjected(decoded_timestamp_column) ||
		               ((bind_data.width == 0 || bind_data.height == 0) &&
		                (IsProjected(width_column) || IsProjected(height_column)));

		unordered_set<idx_t> shards;
		for (idx_t route_index = 0; route_index < bind_data.routes.size(); route_index++) {
			const auto &route = bind_data.routes[route_index];
			routes_by_episode[route.episode_index].push_back(route_index);
			shards.insert(route.video_file_index);
		}
		if (!shards.empty()) {
			max_threads = needs_decode ? std::min<idx_t>(bind_data.decode_threads, bind_data.max_cached_decoders)
			                           : bind_data.decode_threads;
			max_threads = std::min<idx_t>(max_threads, shards.size());
		}

		if (!source_exhausted) {
			frame_connection = make_uniq<Connection>(*context.db);
			frame_result = frame_connection->SendQuery(bind_data.frame_query);
			if (frame_result->HasError()) {
				throw InvalidInputException("Failed to read LeRobot frame timestamps: %s", frame_result->GetError());
			}
		}
#ifdef LEROBOT_HAVE_FFMPEG
		if (needs_decode) {
			decoder_cache = make_uniq<LerobotDecoderCache>(bind_data.max_cached_decoders, metrics);
		}
#endif
	}

	idx_t MaxThreads() const override {
		return max_threads;
	}

	const vector<idx_t> &GetProjectedColumns() const {
		return projected_columns;
	}

	bool NeedsDecode() const {
		return needs_decode;
	}

	LerobotVideoDecodeMetrics &GetMetrics() {
		return metrics;
	}

	bool ClaimBuffer(unique_ptr<LerobotDecodeBuffer> &result) {
		unique_lock<mutex> guard(lock);
		while (true) {
			if (TryClaimReady(result)) {
				return true;
			}
			if (source_exhausted) {
				if (!partial_buffers.empty()) {
					FlushAllPartialBuffers();
					continue;
				}
				return false;
			}
			if (pending_target_count >= bind_data.max_pending_targets) {
				if (FlushOldestPartialBuffer()) {
					continue;
				}
				state_changed.wait(guard);
				continue;
			}
			if (producer_active) {
				state_changed.wait(guard);
				continue;
			}

			producer_active = true;
			guard.unlock();
			vector<LerobotDecodeTarget> targets;
			bool have_source_row;
			try {
				have_source_row = ReadSourceTargets(targets);
			} catch (...) {
				guard.lock();
				producer_active = false;
				state_changed.notify_all();
				throw;
			}
			guard.lock();
			producer_active = false;
			if (have_source_row) {
				QueueTargets(targets);
			} else {
				source_exhausted = true;
			}
			state_changed.notify_all();
		}
	}

	void FinishBuffer(idx_t shard_index) {
		lock_guard<mutex> guard(lock);
		auto erased = busy_shards.erase(shard_index);
		D_ASSERT(erased == 1);
		state_changed.notify_all();
	}

#ifdef LEROBOT_HAVE_FFMPEG
	unique_ptr<LerobotShardDecoder> AcquireDecoder(ClientContext &context, const LerobotDecodeBuffer &buffer) {
		const auto &target = buffer.targets.front();
		const auto &route = bind_data.routes[target.route_index];
		D_ASSERT(route.video_file_index == buffer.shard_index);
		return decoder_cache->Acquire(context, bind_data.GetOptions(), buffer.shard_index,
		                              bind_data.metadata->GetVideoFile(route),
		                              bind_data.metadata->GetVideoFeatureMetadata(route), needs_pixels);
	}

	void ReleaseDecoder(idx_t shard_index, unique_ptr<LerobotShardDecoder> decoder) {
		decoder_cache->Release(shard_index, std::move(decoder));
		FinishBuffer(shard_index);
	}

	void DiscardDecoder(idx_t shard_index, unique_ptr<LerobotShardDecoder> decoder) {
		decoder_cache->Discard(std::move(decoder));
		FinishBuffer(shard_index);
	}
#endif

private:
	bool IsProjected(idx_t logical_column) const {
		return std::find(projected_columns.begin(), projected_columns.end(), logical_column) != projected_columns.end();
	}

	bool TryClaimReady(unique_ptr<LerobotDecodeBuffer> &result) {
		for (auto entry = ready_buffers.begin(); entry != ready_buffers.end(); ++entry) {
			const auto shard_index = (*entry)->shard_index;
			if (busy_shards.find(shard_index) != busy_shards.end()) {
				continue;
			}
			result = std::move(*entry);
			ready_buffers.erase(entry);
			busy_shards.insert(shard_index);
			D_ASSERT(pending_target_count >= result->targets.size());
			pending_target_count -= result->targets.size();
			return true;
		}
		return false;
	}

	bool ReadSourceTargets(vector<LerobotDecodeTarget> &targets) {
		while (!current_chunk || current_row >= current_chunk->size()) {
			current_chunk = frame_result->Fetch();
			current_row = 0;
			if (!current_chunk) {
				if (frame_result->HasError()) {
					throw InvalidInputException("Failed to read LeRobot frame timestamps: %s",
					                            frame_result->GetError());
				}
				return false;
			}
			current_formats.clear();
			current_formats.reserve(current_chunk->ColumnCount());
			for (idx_t column = 0; column < current_chunk->ColumnCount(); column++) {
				current_formats.push_back(UnifiedVectorFormat());
				PrepareUnifiedFormat(current_chunk->data[column], current_chunk->size(), current_formats.back());
			}
		}

		int64_t request_id = 0;
		idx_t request_ordinal = 0;
		idx_t delta_ordinal = 0;
		double delta_timestamp = 0;
		int64_t delta_frame_offset = 0;
		bool is_padding = false;
		int64_t episode_index;
		int64_t frame_index;
		int64_t target_frame_index;
		double frame_timestamp;
		if (bind_data.window_mode) {
			for (idx_t column = 0; column < 9; column++) {
				if (SourceValueIsNull(column)) {
					throw InvalidInputException("LeRobot video window request columns must not contain NULL");
				}
			}
			if (SourceValueIsNull(10)) {
				throw InvalidInputException("LeRobot video window match count must not be NULL");
			}
			const auto match_count = SourceValue<int64_t>(10);
			if (match_count != 1 || SourceValueIsNull(9)) {
				const auto missing_episode = SourceValue<int64_t>(6);
				const auto missing_frame = SourceValue<int64_t>(8);
				if (match_count == 0) {
					throw InvalidInputException("LeRobot episode %d has no Parquet row for frame %d", missing_episode,
					                            missing_frame);
				}
				throw InvalidInputException("LeRobot episode %d has %d Parquet rows for frame %d", missing_episode,
				                            match_count, missing_frame);
			}
			request_id = SourceValue<int64_t>(0);
			const auto request_ordinal_value = SourceValue<int64_t>(1);
			const auto delta_ordinal_value = SourceValue<int64_t>(2);
			if (request_ordinal_value < 0 || delta_ordinal_value < 0) {
				throw InvalidInputException("Invalid LeRobot video window ordinals");
			}
			request_ordinal = static_cast<idx_t>(request_ordinal_value);
			delta_ordinal = static_cast<idx_t>(delta_ordinal_value);
			delta_timestamp = SourceValue<double>(3);
			delta_frame_offset = SourceValue<int64_t>(4);
			is_padding = SourceValue<bool>(5);
			episode_index = SourceValue<int64_t>(6);
			frame_index = SourceValue<int64_t>(7);
			target_frame_index = SourceValue<int64_t>(8);
			frame_timestamp = SourceValue<double>(9);
		} else {
			for (idx_t column = 0; column < 3; column++) {
				if (SourceValueIsNull(column)) {
					throw InvalidInputException("LeRobot frame alignment columns must not contain NULL");
				}
			}
			episode_index = SourceValue<int64_t>(0);
			frame_index = SourceValue<int64_t>(1);
			target_frame_index = frame_index;
			frame_timestamp = SourceValue<double>(2);
		}
		current_row++;
		if (episode_index < 0 || frame_index < 0 || target_frame_index < 0 || !std::isfinite(frame_timestamp) ||
		    frame_timestamp < 0 || !std::isfinite(delta_timestamp)) {
			throw InvalidInputException("Invalid LeRobot frame alignment metadata for episode %d, frame %d",
			                            episode_index, target_frame_index);
		}

		auto route_entry = routes_by_episode.find(episode_index);
		if (route_entry == routes_by_episode.end()) {
			return true;
		}
		for (const auto route_index : route_entry->second) {
			const auto &route = bind_data.routes[route_index];
			const auto video_timestamp = route.from_timestamp + frame_timestamp;
			if (!std::isfinite(video_timestamp) || video_timestamp < route.from_timestamp - bind_data.tolerance ||
			    video_timestamp > route.to_timestamp + bind_data.tolerance) {
				throw InvalidInputException("LeRobot episode %d frame %d timestamp %.6f falls outside video route "
				                            "[%.6f, %.6f] for key '%s'",
				                            episode_index, target_frame_index, video_timestamp, route.from_timestamp,
				                            route.to_timestamp, bind_data.metadata->GetVideoKey(route));
			}
			if (bind_data.window_mode) {
				targets.push_back(LerobotDecodeTarget(
				    request_id, request_ordinal, delta_ordinal, delta_timestamp, delta_frame_offset, is_padding,
				    episode_index, frame_index, target_frame_index, frame_timestamp, video_timestamp, route_index));
			} else {
				targets.push_back(
				    LerobotDecodeTarget(episode_index, frame_index, frame_timestamp, video_timestamp, route_index));
			}
		}
		return true;
	}

	bool SourceValueIsNull(idx_t column) const {
		const auto &format = current_formats[column];
		const auto source_index = format.sel->get_index(current_row);
		return !format.validity.RowIsValid(source_index);
	}

	template <class T>
	T SourceValue(idx_t column) const {
		const auto &format = current_formats[column];
		const auto source_index = format.sel->get_index(current_row);
		return format.GetData<T>()[source_index];
	}

	void QueueTargets(const vector<LerobotDecodeTarget> &targets) {
		metrics.targets.fetch_add(static_cast<uint64_t>(targets.size()), std::memory_order_relaxed);
		for (const auto &target : targets) {
			const auto &route = bind_data.routes[target.route_index];
			auto &partial = partial_buffers[route.video_file_index];
			partial.targets.push_back(target);
			TouchPartialBuffer(route.video_file_index);
			pending_target_count++;
			if (partial.targets.size() >= bind_data.target_buffer_size) {
				FlushPartialBuffer(route.video_file_index);
			}
		}
	}

	void TouchPartialBuffer(idx_t shard_index) {
		auto entry = partial_lru_entries.find(shard_index);
		if (entry != partial_lru_entries.end()) {
			partial_lru.erase(entry->second);
		}
		partial_lru.push_front(shard_index);
		partial_lru_entries[shard_index] = partial_lru.begin();
	}

	void FlushPartialBuffer(idx_t shard_index) {
		auto entry = partial_buffers.find(shard_index);
		if (entry == partial_buffers.end() || entry->second.targets.empty()) {
			return;
		}
		auto buffer = make_uniq<LerobotDecodeBuffer>();
		buffer->shard_index = shard_index;
		buffer->targets = std::move(entry->second.targets);
		partial_buffers.erase(entry);
		auto lru_entry = partial_lru_entries.find(shard_index);
		D_ASSERT(lru_entry != partial_lru_entries.end());
		partial_lru.erase(lru_entry->second);
		partial_lru_entries.erase(lru_entry);
		FinalizeDecodeBuffer(*buffer, bind_data.cluster_gap);
		ready_buffers.push_back(std::move(buffer));
	}

	bool FlushOldestPartialBuffer() {
		if (partial_buffers.empty()) {
			return false;
		}
		D_ASSERT(!partial_lru.empty());
		const auto shard_index = partial_lru.back();
		FlushPartialBuffer(shard_index);
		return true;
	}

	void FlushAllPartialBuffers() {
		while (!partial_buffers.empty()) {
			FlushPartialBuffer(partial_buffers.begin()->first);
		}
	}

private:
	const LerobotVideoFramesBindData &bind_data;
	mutex lock;
	std::condition_variable state_changed;
	unique_ptr<Connection> frame_connection;
	unique_ptr<QueryResult> frame_result;
	bool source_exhausted;
	bool producer_active;
	unique_ptr<DataChunk> current_chunk;
	vector<UnifiedVectorFormat> current_formats;
	idx_t current_row;
	unordered_map<int64_t, vector<idx_t>> routes_by_episode;
	unordered_map<idx_t, LerobotPartialBuffer> partial_buffers;
	list<idx_t> partial_lru;
	unordered_map<idx_t, list<idx_t>::iterator> partial_lru_entries;
	deque<unique_ptr<LerobotDecodeBuffer>> ready_buffers;
	unordered_set<idx_t> busy_shards;
	idx_t pending_target_count;
	idx_t max_threads;
	vector<idx_t> projected_columns;
	bool needs_decode;
	bool needs_pixels;
	LerobotVideoDecodeMetrics metrics;
#ifdef LEROBOT_HAVE_FFMPEG
	unique_ptr<LerobotDecoderCache> decoder_cache;
#endif
};

unique_ptr<GlobalTableFunctionState> LerobotVideoFramesInitGlobal(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoFramesBindData>();
	return make_uniq<LerobotVideoFramesGlobalState>(context, bind_data, input.column_ids);
}

LogicalType GetVideoOutputType(bool window_mode, idx_t logical_column) {
	if (window_mode) {
		switch (logical_column) {
		case LEROBOT_WINDOW_REQUEST_ID:
		case LEROBOT_WINDOW_REQUEST_ORDINAL:
		case LEROBOT_WINDOW_DELTA_ORDINAL:
		case LEROBOT_WINDOW_DELTA_FRAME_OFFSET:
		case LEROBOT_WINDOW_EPISODE_INDEX:
		case LEROBOT_WINDOW_FRAME_INDEX:
		case LEROBOT_WINDOW_TARGET_FRAME_INDEX:
			return LogicalType::BIGINT;
		case LEROBOT_WINDOW_DELTA_TIMESTAMP:
		case LEROBOT_WINDOW_TIMESTAMP:
		case LEROBOT_WINDOW_VIDEO_TIMESTAMP:
		case LEROBOT_WINDOW_DECODED_TIMESTAMP:
			return LogicalType::DOUBLE;
		case LEROBOT_WINDOW_IS_PADDING:
			return LogicalType::BOOLEAN;
		case LEROBOT_WINDOW_VIDEO_KEY:
		case LEROBOT_WINDOW_VIDEO_PATH:
			return LogicalType::VARCHAR;
		case LEROBOT_WINDOW_WIDTH:
		case LEROBOT_WINDOW_HEIGHT:
		case LEROBOT_WINDOW_CHANNELS:
			return LogicalType::INTEGER;
		case LEROBOT_WINDOW_IMAGE:
			return LogicalType::BLOB;
		default:
			throw InternalException("Invalid lerobot_video_windows output column");
		}
	}
	switch (logical_column) {
	case LEROBOT_FRAME_EPISODE_INDEX:
	case LEROBOT_FRAME_FRAME_INDEX:
		return LogicalType::BIGINT;
	case LEROBOT_FRAME_TIMESTAMP:
	case LEROBOT_FRAME_VIDEO_TIMESTAMP:
	case LEROBOT_FRAME_DECODED_TIMESTAMP:
		return LogicalType::DOUBLE;
	case LEROBOT_FRAME_VIDEO_KEY:
	case LEROBOT_FRAME_VIDEO_PATH:
		return LogicalType::VARCHAR;
	case LEROBOT_FRAME_WIDTH:
	case LEROBOT_FRAME_HEIGHT:
	case LEROBOT_FRAME_CHANNELS:
		return LogicalType::INTEGER;
	case LEROBOT_FRAME_IMAGE:
		return LogicalType::BLOB;
	default:
		throw InternalException("Invalid lerobot_video_frames output column");
	}
}

unique_ptr<Expression> BuildVideoFilterExpression(const LerobotVideoFramesBindData &bind_data,
                                                  TableFunctionInitInput &input) {
	if (!input.filters) {
		return nullptr;
	}
	auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
	for (const auto &entry : input.filters->filters) {
		const auto output_index = entry.first;
		if (output_index >= input.column_ids.size()) {
			throw InternalException("Invalid LeRobot pushed-down filter column");
		}
		const auto logical_column = static_cast<idx_t>(input.column_ids[output_index]);
		BoundReferenceExpression column(GetVideoOutputType(bind_data.window_mode, logical_column), output_index);
		conjunction->children.push_back(entry.second->ToExpression(column));
	}
	if (conjunction->children.empty()) {
		return nullptr;
	}
	if (conjunction->children.size() == 1) {
		auto result = std::move(conjunction->children.front());
		return result;
	}
	return std::move(conjunction);
}

struct LerobotVideoFramesLocalState final : public LocalTableFunctionState {
	LerobotVideoFramesLocalState(ExecutionContext &context, const LerobotVideoFramesBindData &bind_data,
	                             TableFunctionInitInput &input)
	    : target_position(0), rows_scanned(0), have_decoded(false), filter_selection(STANDARD_VECTOR_SIZE) {
		filter_expression = BuildVideoFilterExpression(bind_data, input);
		if (filter_expression) {
			filter_executor = make_uniq<ExpressionExecutor>(context.client, *filter_expression);
		}
	}

	void ApplyFilters(DataChunk &output) {
		if (!filter_executor || output.size() == 0) {
			return;
		}
		const auto approved = filter_executor->SelectExpression(output, filter_selection);
		if (approved != output.size()) {
			output.Slice(filter_selection, approved);
		}
	}

	unique_ptr<LerobotDecodeBuffer> buffer;
	idx_t target_position;
	idx_t rows_scanned;
#ifdef LEROBOT_HAVE_FFMPEG
	unique_ptr<LerobotShardDecoder> decoder;
#endif
	DecodedVideoFrame decoded;
	bool have_decoded;
	unique_ptr<Expression> filter_expression;
	unique_ptr<ExpressionExecutor> filter_executor;
	SelectionVector filter_selection;
};

unique_ptr<LocalTableFunctionState>
LerobotVideoFramesInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoFramesBindData>();
	return make_uniq<LerobotVideoFramesLocalState>(context, bind_data, input);
}

void WriteInt64(Vector &vector, idx_t row, int64_t value) {
	GetMutableFlatData<int64_t>(vector)[row] = value;
}

void WriteInt32(Vector &vector, idx_t row, int32_t value) {
	GetMutableFlatData<int32_t>(vector)[row] = value;
}

void WriteDouble(Vector &vector, idx_t row, double value) {
	GetMutableFlatData<double>(vector)[row] = value;
}

void WriteBoolean(Vector &vector, idx_t row, bool value) {
	GetMutableFlatData<bool>(vector)[row] = value;
}

void WriteString(Vector &vector, idx_t row, const string &value) {
	GetMutableFlatData<string_t>(vector)[row] = StringVector::AddString(vector, value);
}

void WriteBlob(Vector &vector, idx_t row, const string &value) {
	GetMutableFlatData<string_t>(vector)[row] = StringVector::AddStringOrBlob(vector, value.data(), value.size());
}

int32_t GetOutputWidth(const LerobotVideoFramesBindData &bind_data, const DecodedVideoFrame *decoded) {
	if (bind_data.width > 0) {
		return bind_data.width;
	}
	D_ASSERT(decoded);
	return decoded->width;
}

int32_t GetOutputHeight(const LerobotVideoFramesBindData &bind_data, const DecodedVideoFrame *decoded) {
	if (bind_data.height > 0) {
		return bind_data.height;
	}
	D_ASSERT(decoded);
	return decoded->height;
}

void WriteLegacyColumn(const LerobotVideoFramesBindData &bind_data, const LerobotDecodeTarget &target,
                       const LerobotVideoRoute &route, const DecodedVideoFrame *decoded, idx_t logical_column,
                       idx_t row, Vector &output) {
	switch (logical_column) {
	case LEROBOT_FRAME_EPISODE_INDEX:
		WriteInt64(output, row, target.episode_index);
		break;
	case LEROBOT_FRAME_FRAME_INDEX:
		WriteInt64(output, row, target.frame_index);
		break;
	case LEROBOT_FRAME_TIMESTAMP:
		WriteDouble(output, row, target.frame_timestamp);
		break;
	case LEROBOT_FRAME_VIDEO_KEY:
		WriteString(output, row, bind_data.metadata->GetVideoKey(route));
		break;
	case LEROBOT_FRAME_VIDEO_PATH:
		WriteString(output, row, bind_data.metadata->GetVideoFile(route));
		break;
	case LEROBOT_FRAME_VIDEO_TIMESTAMP:
		WriteDouble(output, row, target.video_timestamp);
		break;
	case LEROBOT_FRAME_DECODED_TIMESTAMP:
		D_ASSERT(decoded);
		WriteDouble(output, row, decoded->decoded_timestamp);
		break;
	case LEROBOT_FRAME_WIDTH:
		WriteInt32(output, row, GetOutputWidth(bind_data, decoded));
		break;
	case LEROBOT_FRAME_HEIGHT:
		WriteInt32(output, row, GetOutputHeight(bind_data, decoded));
		break;
	case LEROBOT_FRAME_CHANNELS:
		WriteInt32(output, row, bind_data.metadata->GetVideoFeatureMetadata(route).is_depth_map ? 1 : 3);
		break;
	case LEROBOT_FRAME_IMAGE:
		D_ASSERT(decoded && decoded->pixels);
		WriteBlob(output, row, *decoded->pixels);
		break;
	default:
		throw InternalException("Invalid lerobot_video_frames projected column");
	}
}

void WriteWindowColumn(const LerobotVideoFramesBindData &bind_data, const LerobotDecodeTarget &target,
                       const LerobotVideoRoute &route, const DecodedVideoFrame *decoded, idx_t logical_column,
                       idx_t row, Vector &output) {
	switch (logical_column) {
	case LEROBOT_WINDOW_REQUEST_ID:
		WriteInt64(output, row, target.request_id);
		break;
	case LEROBOT_WINDOW_REQUEST_ORDINAL:
		WriteInt64(output, row, static_cast<int64_t>(target.request_ordinal));
		break;
	case LEROBOT_WINDOW_DELTA_ORDINAL:
		WriteInt64(output, row, static_cast<int64_t>(target.delta_ordinal));
		break;
	case LEROBOT_WINDOW_DELTA_TIMESTAMP:
		WriteDouble(output, row, target.delta_timestamp);
		break;
	case LEROBOT_WINDOW_DELTA_FRAME_OFFSET:
		WriteInt64(output, row, target.delta_frame_offset);
		break;
	case LEROBOT_WINDOW_IS_PADDING:
		WriteBoolean(output, row, target.is_padding);
		break;
	case LEROBOT_WINDOW_EPISODE_INDEX:
		WriteInt64(output, row, target.episode_index);
		break;
	case LEROBOT_WINDOW_FRAME_INDEX:
		WriteInt64(output, row, target.frame_index);
		break;
	case LEROBOT_WINDOW_TARGET_FRAME_INDEX:
		WriteInt64(output, row, target.target_frame_index);
		break;
	case LEROBOT_WINDOW_TIMESTAMP:
		WriteDouble(output, row, target.frame_timestamp);
		break;
	case LEROBOT_WINDOW_VIDEO_KEY:
		WriteString(output, row, bind_data.metadata->GetVideoKey(route));
		break;
	case LEROBOT_WINDOW_VIDEO_PATH:
		WriteString(output, row, bind_data.metadata->GetVideoFile(route));
		break;
	case LEROBOT_WINDOW_VIDEO_TIMESTAMP:
		WriteDouble(output, row, target.video_timestamp);
		break;
	case LEROBOT_WINDOW_DECODED_TIMESTAMP:
		D_ASSERT(decoded);
		WriteDouble(output, row, decoded->decoded_timestamp);
		break;
	case LEROBOT_WINDOW_WIDTH:
		WriteInt32(output, row, GetOutputWidth(bind_data, decoded));
		break;
	case LEROBOT_WINDOW_HEIGHT:
		WriteInt32(output, row, GetOutputHeight(bind_data, decoded));
		break;
	case LEROBOT_WINDOW_CHANNELS:
		WriteInt32(output, row, bind_data.metadata->GetVideoFeatureMetadata(route).is_depth_map ? 1 : 3);
		break;
	case LEROBOT_WINDOW_IMAGE:
		D_ASSERT(decoded && decoded->pixels);
		WriteBlob(output, row, *decoded->pixels);
		break;
	default:
		throw InternalException("Invalid lerobot_video_windows projected column");
	}
}

int32_t GetTargetOutputWidth(const LerobotVideoTargetsBindData &bind_data, const DecodedVideoFrame *decoded) {
	if (bind_data.options.width > 0) {
		return bind_data.options.width;
	}
	D_ASSERT(decoded);
	return decoded->width;
}

int32_t GetTargetOutputHeight(const LerobotVideoTargetsBindData &bind_data, const DecodedVideoFrame *decoded) {
	if (bind_data.options.height > 0) {
		return bind_data.options.height;
	}
	D_ASSERT(decoded);
	return decoded->height;
}

void WriteTargetRelationColumn(const LerobotVideoTargetsBindData &bind_data, const LerobotDecodeTarget &target,
                               const LerobotVideoRoute &route, const DecodedVideoFrame *decoded, idx_t logical_column,
                               idx_t row, Vector &output) {
	switch (logical_column) {
	case LEROBOT_TARGET_REQUEST_ID:
		WriteInt64(output, row, target.request_id);
		break;
	case LEROBOT_TARGET_ORDINAL:
		WriteInt64(output, row, static_cast<int64_t>(target.request_ordinal));
		break;
	case LEROBOT_TARGET_EPISODE_INDEX:
		WriteInt64(output, row, target.episode_index);
		break;
	case LEROBOT_TARGET_FRAME_INDEX:
		WriteInt64(output, row, target.frame_index);
		break;
	case LEROBOT_TARGET_VIDEO_KEY:
		WriteString(output, row, bind_data.metadata->GetVideoKey(route));
		break;
	case LEROBOT_TARGET_DELTA_INDEX:
		WriteInt64(output, row, static_cast<int64_t>(target.delta_ordinal));
		break;
	case LEROBOT_TARGET_DELTA_TIMESTAMP:
		WriteDouble(output, row, target.delta_timestamp);
		break;
	case LEROBOT_TARGET_DELTA_FRAME_OFFSET:
		WriteInt64(output, row, target.delta_frame_offset);
		break;
	case LEROBOT_TARGET_IS_PADDING:
		WriteBoolean(output, row, target.is_padding);
		break;
	case LEROBOT_TARGET_FRAME_INDEX_RESOLVED:
		WriteInt64(output, row, target.target_frame_index);
		break;
	case LEROBOT_TARGET_TIMESTAMP:
		WriteDouble(output, row, target.frame_timestamp);
		break;
	case LEROBOT_TARGET_VIDEO_PATH:
		WriteString(output, row, bind_data.metadata->GetVideoFile(route));
		break;
	case LEROBOT_TARGET_VIDEO_TIMESTAMP:
		WriteDouble(output, row, target.video_timestamp);
		break;
	case LEROBOT_TARGET_DECODED_TIMESTAMP:
		D_ASSERT(decoded);
		WriteDouble(output, row, decoded->decoded_timestamp);
		break;
	case LEROBOT_TARGET_WIDTH:
		WriteInt32(output, row, GetTargetOutputWidth(bind_data, decoded));
		break;
	case LEROBOT_TARGET_HEIGHT:
		WriteInt32(output, row, GetTargetOutputHeight(bind_data, decoded));
		break;
	case LEROBOT_TARGET_CHANNELS:
		WriteInt32(output, row, bind_data.metadata->GetVideoFeatureMetadata(route).is_depth_map ? 1 : 3);
		break;
	case LEROBOT_TARGET_IMAGE:
		D_ASSERT(decoded && decoded->pixels);
		WriteBlob(output, row, *decoded->pixels);
		break;
	default:
		throw InternalException("Invalid lerobot_video_targets projected column");
	}
}

void WriteTargetRelationRow(const LerobotVideoTargetsBindData &bind_data,
                            const LerobotVideoTargetsLocalState &local_state, const LerobotDecodeTarget &target,
                            const DecodedVideoFrame *decoded, const vector<idx_t> &projected_columns, idx_t row,
                            DataChunk &output) {
	const auto &route = local_state.routes[target.route_index];
	for (idx_t output_column = 0; output_column < projected_columns.size(); output_column++) {
		WriteTargetRelationColumn(bind_data, target, route, decoded, projected_columns[output_column], row,
		                          output.data[output_column]);
	}
}

void WriteVideoTarget(const LerobotVideoFramesBindData &bind_data, const LerobotDecodeTarget &target,
                      const DecodedVideoFrame *decoded, const vector<idx_t> &projected_columns, idx_t row,
                      DataChunk &output) {
	const auto &route = bind_data.routes[target.route_index];
	for (idx_t output_column = 0; output_column < projected_columns.size(); output_column++) {
		if (bind_data.window_mode) {
			WriteWindowColumn(bind_data, target, route, decoded, projected_columns[output_column], row,
			                  output.data[output_column]);
		} else {
			WriteLegacyColumn(bind_data, target, route, decoded, projected_columns[output_column], row,
			                  output.data[output_column]);
		}
	}
}

bool ProduceLerobotVideoFrames(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoFramesBindData>();
	auto &global_state = input.global_state->Cast<LerobotVideoFramesGlobalState>();
	auto &local_state = input.local_state->Cast<LerobotVideoFramesLocalState>();
	idx_t count = 0;
	idx_t output_bytes = 0;
	const auto &projected_columns = global_state.GetProjectedColumns();

	while (count < bind_data.output_batch_size) {
		if (!local_state.buffer) {
			if (!global_state.ClaimBuffer(local_state.buffer)) {
				break;
			}
			local_state.target_position = 0;
			local_state.have_decoded = false;
			if (!global_state.NeedsDecode()) {
				continue;
			}
#ifndef LEROBOT_HAVE_FFMPEG
			global_state.FinishBuffer(local_state.buffer->shard_index);
			local_state.buffer.reset();
			throw MissingExtensionException(
			    "LeRobot video pixel decoding requires FFmpeg development libraries at extension build time; rebuild "
			    "lerobot with LEROBOT_ENABLE_FFMPEG=ON");
#else
			try {
				local_state.decoder = global_state.AcquireDecoder(context, *local_state.buffer);
				local_state.decoder->BeginBuffer(*local_state.buffer);
			} catch (...) {
				if (local_state.decoder) {
					global_state.DiscardDecoder(local_state.buffer->shard_index, std::move(local_state.decoder));
				} else {
					global_state.FinishBuffer(local_state.buffer->shard_index);
				}
				local_state.buffer.reset();
				throw;
			}
#endif
		}

		if (!global_state.NeedsDecode()) {
			while (local_state.target_position < local_state.buffer->targets.size() &&
			       count < bind_data.output_batch_size) {
				const auto &target = local_state.buffer->targets[local_state.target_position++];
				WriteVideoTarget(bind_data, target, nullptr, projected_columns, count, output);
				count++;
			}
			if (local_state.target_position >= local_state.buffer->targets.size()) {
				global_state.FinishBuffer(local_state.buffer->shard_index);
				local_state.buffer.reset();
			}
			continue;
		}

#ifdef LEROBOT_HAVE_FFMPEG
		bool decoded_frame;
		if (!local_state.have_decoded) {
			try {
				decoded_frame = local_state.decoder->Next(local_state.decoded);
			} catch (...) {
				global_state.DiscardDecoder(local_state.buffer->shard_index, std::move(local_state.decoder));
				local_state.buffer.reset();
				throw;
			}
			if (!decoded_frame) {
				const auto shard_index = local_state.buffer->shard_index;
				global_state.ReleaseDecoder(shard_index, std::move(local_state.decoder));
				local_state.buffer.reset();
				continue;
			}
			local_state.have_decoded = true;
		}
		const auto pixel_bytes = local_state.decoded.pixels ? local_state.decoded.pixels->size() : 0;
		if (count > 0 &&
		    pixel_bytes > bind_data.max_output_bytes - std::min(output_bytes, bind_data.max_output_bytes)) {
			break;
		}
		const auto &target = local_state.buffer->targets[local_state.decoded.target_index];
		WriteVideoTarget(bind_data, target, &local_state.decoded, projected_columns, count, output);
		output_bytes += pixel_bytes;
		local_state.have_decoded = false;
		count++;
#endif
	}
	local_state.rows_scanned += count;
	SetOutputCardinality(output, count);
	return count == 0;
}

void LerobotVideoFramesFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &local_state = input.local_state->Cast<LerobotVideoFramesLocalState>();
	while (true) {
		const auto exhausted = ProduceLerobotVideoFrames(context, input, output);
		local_state.ApplyFilters(output);
		if (output.size() > 0 || exhausted) {
			return;
		}
		output.Reset();
	}
}

OperatorResultType LerobotVideoTargetsFunction(ExecutionContext &context, TableFunctionInput &input,
                                               DataChunk &target_input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoTargetsBindData>();
	auto &global_state = input.global_state->Cast<LerobotVideoTargetsGlobalState>();
	auto &local_state = input.local_state->Cast<LerobotVideoTargetsLocalState>();
	if (!local_state.input_initialized) {
		local_state.InitializeInput(target_input);
	}
	idx_t count = 0;
	idx_t output_bytes = 0;
	const auto &projected_columns = global_state.GetProjectedColumns();

	while (count < bind_data.options.output_batch_size) {
		if (!local_state.buffer) {
			if (local_state.buffers.empty()) {
				if (local_state.input_position >= target_input.size()) {
					break;
				}
				BuildTargetBuffers(context.client, bind_data, global_state, local_state, target_input);
				continue;
			}
			local_state.buffer = std::move(local_state.buffers.front());
			local_state.buffers.pop_front();
			local_state.target_position = 0;
			local_state.have_decoded = false;
			if (!global_state.NeedsDecode()) {
				continue;
			}
#ifndef LEROBOT_HAVE_FFMPEG
			local_state.buffer.reset();
			throw MissingExtensionException(
			    "LeRobot video pixel decoding requires FFmpeg development libraries at extension build time; rebuild "
			    "lerobot with LEROBOT_ENABLE_FFMPEG=ON");
#else
			try {
				const auto &first_target = local_state.buffer->targets.front();
				const auto &route = local_state.routes[first_target.route_index];
				local_state.decoder = global_state.AcquireDecoder(context.client, route);
				local_state.decoder->BeginBuffer(*local_state.buffer);
			} catch (...) {
				if (local_state.decoder) {
					global_state.DiscardDecoder(std::move(local_state.decoder));
				}
				local_state.buffer.reset();
				throw;
			}
#endif
		}

		if (!global_state.NeedsDecode()) {
			while (local_state.target_position < local_state.buffer->targets.size() &&
			       count < bind_data.options.output_batch_size) {
				const auto &target = local_state.buffer->targets[local_state.target_position++];
				WriteTargetRelationRow(bind_data, local_state, target, nullptr, projected_columns, count, output);
				count++;
			}
			if (local_state.target_position >= local_state.buffer->targets.size()) {
				local_state.buffer.reset();
			}
			continue;
		}

#ifdef LEROBOT_HAVE_FFMPEG
		bool decoded_frame;
		if (!local_state.have_decoded) {
			try {
				decoded_frame = local_state.decoder->Next(local_state.decoded);
			} catch (...) {
				global_state.DiscardDecoder(std::move(local_state.decoder));
				local_state.buffer.reset();
				throw;
			}
			if (!decoded_frame) {
				const auto shard_index = local_state.buffer->shard_index;
				global_state.ReleaseDecoder(shard_index, std::move(local_state.decoder));
				local_state.buffer.reset();
				continue;
			}
			local_state.have_decoded = true;
		}
		const auto pixel_bytes = local_state.decoded.pixels ? local_state.decoded.pixels->size() : 0;
		if (count > 0 && pixel_bytes > bind_data.options.max_output_bytes -
		                                   std::min(output_bytes, bind_data.options.max_output_bytes)) {
			break;
		}
		const auto &target = local_state.buffer->targets[local_state.decoded.target_index];
		WriteTargetRelationRow(bind_data, local_state, target, &local_state.decoded, projected_columns, count, output);
		output_bytes += pixel_bytes;
		local_state.have_decoded = false;
		count++;
#endif
	}
	SetOutputCardinality(output, count);
	if (local_state.InputFinished(target_input)) {
		local_state.FinishInput();
		return OperatorResultType::NEED_MORE_INPUT;
	}
	return OperatorResultType::HAVE_MORE_OUTPUT;
}

idx_t LerobotVideoFramesRowsScanned(GlobalTableFunctionState &, LocalTableFunctionState &local_state) {
	return local_state.Cast<LerobotVideoFramesLocalState>().rows_scanned;
}

void AddLerobotVideoMetrics(InsertionOrderPreservingMap<string> &result, const LerobotVideoDecodeMetrics &metrics) {
	result["LeRobot Targets"] = std::to_string(metrics.targets.load());
	result["LeRobot Decoder Acquires"] = std::to_string(metrics.decoder_acquires.load());
	result["LeRobot Decoder Cache Hits"] = std::to_string(metrics.decoder_cache_hits.load());
	result["LeRobot Decoder Opens"] = std::to_string(metrics.decoder_opens.load());
	result["LeRobot Decoder Evictions"] = std::to_string(metrics.decoder_evictions.load());
	result["LeRobot Decoder Seeks"] = std::to_string(metrics.decoder_seeks.load());
	result["LeRobot AVIO Seeks"] = std::to_string(metrics.avio_seeks.load());
	result["LeRobot Video Bytes Read"] = std::to_string(metrics.video_bytes_read.load());
	result["LeRobot Frames Decoded"] = std::to_string(metrics.frames_decoded.load());
	result["LeRobot RGB Conversions"] = std::to_string(metrics.rgb_conversions.load());
	result["LeRobot RGB Fan-out Hits"] = std::to_string(metrics.rgb_fanout_hits.load());
	result["LeRobot Depth Conversions"] = std::to_string(metrics.depth_conversions.load());
	result["LeRobot Depth Fan-out Hits"] = std::to_string(metrics.depth_fanout_hits.load());
}

InsertionOrderPreservingMap<string> LerobotVideoFramesDynamicToString(TableFunctionDynamicToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	if (input.global_state) {
		auto &metrics = input.global_state->Cast<LerobotVideoFramesGlobalState>().GetMetrics();
		AddLerobotVideoMetrics(result, metrics);
	}
	return result;
}

InsertionOrderPreservingMap<string> LerobotVideoTargetsDynamicToString(TableFunctionDynamicToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	if (input.global_state) {
		auto &metrics = input.global_state->Cast<LerobotVideoTargetsGlobalState>().GetMetrics();
		AddLerobotVideoMetrics(result, metrics);
	}
	return result;
}

void AddVideoDecodeNamedParameters(TableFunction &function, bool include_video_keys = true,
                                   bool source_function = true) {
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
	// Compatibility alias retained for queries written before the cache and
	// worker limits became independently configurable.
	function.named_parameters["max_open_shards"] = LogicalType::BIGINT;
	function.named_parameters["decode_threads"] = LogicalType::BIGINT;
	function.named_parameters["max_pending_targets"] = LogicalType::BIGINT;
	function.named_parameters["max_output_bytes"] = LogicalType::BIGINT;
	function.named_parameters["codec_threads"] = LogicalType::BIGINT;
	function.named_parameters["depth_output_unit"] = LogicalType::VARCHAR;
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	function.projection_pushdown = true;
	if (source_function) {
		function.filter_pushdown = true;
		function.rows_scanned = LerobotVideoFramesRowsScanned;
		function.dynamic_to_string = LerobotVideoFramesDynamicToString;
	}
}

} // namespace

TableFunctionSet LerobotFunctions::GetVideoFramesFunction() {
	TableFunction function("lerobot_video_frames", {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::BIGINT)},
	                       LerobotVideoFramesFunction, LerobotVideoFramesBind, LerobotVideoFramesInitGlobal,
	                       LerobotVideoFramesInitLocal);
	function.named_parameters["frame_indices"] = LogicalType::LIST(LogicalType::BIGINT);
	AddVideoDecodeNamedParameters(function);
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetVideoWindowsFunction() {
	auto request_type = LogicalType::STRUCT({{"request_id", LogicalType::BIGINT},
	                                         {"episode_index", LogicalType::BIGINT},
	                                         {"frame_index", LogicalType::BIGINT}});
	TableFunction function("lerobot_video_windows", {LogicalType::VARCHAR, LogicalType::LIST(request_type)},
	                       LerobotVideoFramesFunction, LerobotVideoWindowsBind, LerobotVideoFramesInitGlobal,
	                       LerobotVideoFramesInitLocal);
	function.named_parameters["delta_timestamps"] = LogicalType::LIST(LogicalType::DOUBLE);
	AddVideoDecodeNamedParameters(function);
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetVideoTargetsFunction() {
	TableFunction function("lerobot_video_targets", {LogicalType::VARCHAR, LogicalType::TABLE}, nullptr,
	                       LerobotVideoTargetsBind, LerobotVideoTargetsInitGlobal, LerobotVideoTargetsInitLocal);
	function.in_out_function = LerobotVideoTargetsFunction;
	function.named_parameters["delta_timestamps"] = LogicalType::LIST(LogicalType::DOUBLE);
	AddVideoDecodeNamedParameters(function, false, false);
	function.dynamic_to_string = LerobotVideoTargetsDynamicToString;
	return TableFunctionSet(std::move(function));
}

} // namespace duckdb
