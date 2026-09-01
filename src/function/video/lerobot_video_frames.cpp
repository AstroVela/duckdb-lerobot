#include "function/lerobot_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/list.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/query_result.hpp"

#include "function/lerobot_multi_file_reader.hpp"
#include "storage/lerobot_metadata_cache.hpp"

#if defined(__has_include)
#if __has_include("duckdb/common/vector/flat_vector.hpp")
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#endif
#endif

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <type_traits>
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
static const idx_t LEROBOT_DEFAULT_MAX_OPEN_SHARDS = 8;
static const idx_t LEROBOT_DEFAULT_DECODE_THREADS = 8;
static const idx_t LEROBOT_DEFAULT_MAX_OUTPUT_BYTES = 64 * 1024 * 1024;
static const int64_t LEROBOT_DEFAULT_CODEC_THREADS = 1;
static const idx_t LEROBOT_MAX_TARGET_BUFFER_SIZE = 1024 * 1024;
static const idx_t LEROBOT_MAX_OPEN_SHARDS = 1024;
static const idx_t LEROBOT_MAX_DECODE_THREADS = 1024;
static const idx_t LEROBOT_MAX_PENDING_TARGETS = 10 * 1024 * 1024;
static const idx_t LEROBOT_MAX_WINDOW_TARGETS = 100000;
static const idx_t LEROBOT_DECODE_FRAME_BUDGET = 20000;
static const double LEROBOT_DEFAULT_CLUSTER_GAP_SECONDS = 10.0;
static const double LEROBOT_DEFAULT_TOLERANCE_SECONDS = 1e-4;

template <typename CALLBACK>
struct BindColumnNames;

template <typename RESULT, typename CONTEXT, typename INPUT, typename RETURN_TYPES, typename COLUMN_NAMES>
struct BindColumnNames<RESULT (*)(CONTEXT, INPUT, RETURN_TYPES, COLUMN_NAMES)> {
	using type = typename std::remove_reference<COLUMN_NAMES>::type;
};

using LerobotColumnNames = typename BindColumnNames<table_function_bind_t>::type;

template <typename FLAT_VECTOR, typename T>
auto GetMutableFlatDataInternal(Vector &vector, int) -> decltype(FLAT_VECTOR::template GetDataMutable<T>(vector)) {
	return FLAT_VECTOR::template GetDataMutable<T>(vector);
}

template <typename FLAT_VECTOR, typename T>
auto GetMutableFlatDataInternal(Vector &vector, long) -> decltype(FLAT_VECTOR::template GetData<T>(vector)) {
	return FLAT_VECTOR::template GetData<T>(vector);
}

template <typename T>
T *GetMutableFlatData(Vector &vector) {
	return GetMutableFlatDataInternal<FlatVector, T>(vector, 0);
}

template <typename CHUNK>
auto SetOutputCardinality(CHUNK &output, idx_t count, int) -> decltype(output.SetCardinalityUnsafe(count), void()) {
	output.SetCardinalityUnsafe(count);
}

template <typename CHUNK>
void SetOutputCardinality(CHUNK &output, idx_t count, long) {
	output.SetCardinality(count);
}

template <typename CONTEXT>
auto CheckForInterrupt(CONTEXT &context, int) -> decltype(context.InterruptCheck(), void()) {
	context.InterruptCheck();
}

template <typename CONTEXT>
void CheckForInterrupt(CONTEXT &, long) {
}

bool GetRefreshParameter(TableFunctionBindInput &input) {
	auto entry = input.named_parameters.find("refresh");
	if (entry == input.named_parameters.end()) {
		return false;
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_video_frames refresh must not be NULL");
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

struct LerobotVideoOptions {
	double tolerance;
	double cluster_gap;
	int32_t width;
	int32_t height;
	idx_t output_batch_size;
	idx_t target_buffer_size;
	idx_t max_open_shards;
	idx_t decode_threads;
	idx_t max_pending_targets;
	idx_t max_output_bytes;
	int32_t codec_threads;
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

	const auto tolerance = GetNamedDouble(input, "tolerance", LEROBOT_DEFAULT_TOLERANCE_SECONDS);
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
	const auto max_open_shards_value = GetNamedInteger(input, "max_open_shards", LEROBOT_DEFAULT_MAX_OPEN_SHARDS);
	if (max_open_shards_value <= 0 || max_open_shards_value > static_cast<int64_t>(LEROBOT_MAX_OPEN_SHARDS)) {
		throw BinderException("%s max_open_shards must be between 1 and %d", function_name, LEROBOT_MAX_OPEN_SHARDS);
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

	LerobotVideoOptions result;
	result.tolerance = tolerance;
	result.cluster_gap = cluster_gap;
	result.width = static_cast<int32_t>(width_value);
	result.height = static_cast<int32_t>(height_value);
	result.output_batch_size = static_cast<idx_t>(batch_size_value);
	result.target_buffer_size = static_cast<idx_t>(target_buffer_size_value);
	result.max_open_shards = static_cast<idx_t>(max_open_shards_value);
	result.decode_threads = static_cast<idx_t>(decode_threads_value);
	result.max_pending_targets = max_pending_targets;
	result.max_output_bytes = static_cast<idx_t>(max_output_bytes_value);
	result.codec_threads = static_cast<int32_t>(codec_threads_value);
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
	      target_buffer_size(options.target_buffer_size), max_open_shards(options.max_open_shards),
	      decode_threads(options.decode_threads), max_pending_targets(options.max_pending_targets),
	      max_output_bytes(options.max_output_bytes), codec_threads(options.codec_threads) {
	}

	unique_ptr<FunctionData> Copy() const override {
		LerobotVideoOptions options;
		options.tolerance = tolerance;
		options.cluster_gap = cluster_gap;
		options.width = width;
		options.height = height;
		options.output_batch_size = output_batch_size;
		options.target_buffer_size = target_buffer_size;
		options.max_open_shards = max_open_shards;
		options.decode_threads = decode_threads;
		options.max_pending_targets = max_pending_targets;
		options.max_output_bytes = max_output_bytes;
		options.codec_threads = codec_threads;
		return make_uniq<LerobotVideoFramesBindData>(metadata, routes, frame_query, window_mode, options);
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
	idx_t max_open_shards;
	idx_t decode_threads;
	idx_t max_pending_targets;
	idx_t max_output_bytes;
	int32_t codec_threads;
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
                                                vector<LogicalType> &return_types, LerobotColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_video_frames root must not be NULL");
	}
	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	auto episode_indices = GetNonNegativeIndices(input.inputs[1], "episode_indices");
	const auto refresh = GetRefreshParameter(input);

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

struct LerobotVideoWindowDelta {
	double timestamp;
	int64_t frame_offset;
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

vector<LerobotVideoWindowDelta> GetVideoWindowDeltas(TableFunctionBindInput &input, int64_t fps, double tolerance) {
	vector<double> timestamps;
	auto entry = input.named_parameters.find("delta_timestamps");
	if (entry == input.named_parameters.end()) {
		timestamps.push_back(0);
	} else {
		if (entry->second.IsNull()) {
			throw BinderException("lerobot_video_windows delta_timestamps must not be NULL");
		}
		for (const auto &child : ListValue::GetChildren(entry->second)) {
			if (child.IsNull()) {
				throw BinderException("lerobot_video_windows delta_timestamps must not contain NULL");
			}
			timestamps.push_back(child.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>());
		}
	}

	vector<LerobotVideoWindowDelta> result;
	result.reserve(timestamps.size());
	for (const auto timestamp : timestamps) {
		if (!std::isfinite(timestamp)) {
			throw BinderException("lerobot_video_windows delta_timestamps must be finite");
		}
		const long double scaled = static_cast<long double>(timestamp) * static_cast<long double>(fps);
		if (scaled < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
		    scaled > static_cast<long double>(std::numeric_limits<int64_t>::max())) {
			throw BinderException("lerobot_video_windows delta timestamp %.17g is too large", timestamp);
		}
		const auto frame_offset = static_cast<int64_t>(std::llround(scaled));
		const auto canonical_timestamp = static_cast<double>(frame_offset) / static_cast<double>(fps);
		if (std::fabs(timestamp - canonical_timestamp) > tolerance) {
			throw BinderException("lerobot_video_windows delta timestamp %.17g is not a multiple of 1/fps (%d) "
			                      "within tolerance %.17g",
			                      timestamp, fps, tolerance);
		}
		LerobotVideoWindowDelta delta;
		delta.timestamp = timestamp;
		delta.frame_offset = frame_offset;
		result.push_back(delta);
	}
	return result;
}

string BuildVideoWindowQuery(const vector<LerobotVideoWindowRequest> &requests,
                             const vector<LerobotVideoWindowDelta> &deltas,
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
                                                 vector<LogicalType> &return_types, LerobotColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_video_windows root must not be NULL");
	}
	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	auto requests = GetVideoWindowRequests(input.inputs[1]);

	bool cache_hit;
	auto video_metadata = LerobotVideoMetadata::Get(context, root, GetRefreshParameter(input), cache_hit);
	auto video_keys = GetVideoKeys(input, *video_metadata);
	auto options = GetVideoOptions(input, "lerobot_video_windows");
	auto deltas = GetVideoWindowDeltas(input, video_metadata->GetFPS(), options.tolerance);
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
	DuckDBAVIOState(ClientContext &context, const string &path)
	    : handle(FileSystem::GetFileSystem(context).OpenFile(path, FileFlags::FILE_FLAGS_READ)), position(0) {
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
};

class LerobotShardDecoder {
public:
	LerobotShardDecoder(ClientContext &context_p, const LerobotVideoFramesBindData &bind_data_p, idx_t shard_index_p,
	                    string video_path_p, bool needs_pixels_p)
	    : context(context_p), bind_data(bind_data_p), shard_index(shard_index_p), video_path(std::move(video_path_p)),
	      needs_pixels(needs_pixels_p), buffer(nullptr), io_state(context_p, video_path), format_context(nullptr),
	      avio_context(nullptr), codec_context(nullptr), packet(nullptr), previous_frame(nullptr),
	      current_frame(nullptr), video_stream(nullptr), sws_context(nullptr), cluster_position(0), target_position(0),
	      decoded_frames_in_buffer(0), demux_eof(false), flush_sent(false), decoder_eof(false), have_previous(false),
	      have_current(false), previous_timestamp(0), current_timestamp(0), have_last_target(false),
	      last_target_timestamp(0), have_converted_frame(false), converted_timestamp(0), converted_source_width(0),
	      converted_source_height(0), converted_source_format(AV_PIX_FMT_NONE), sws_source_width(0),
	      sws_source_height(0), sws_source_format(AV_PIX_FMT_NONE), resize_source_width(0), resize_source_height(0),
	      resize_target_width(0), resize_target_height(0) {
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
		                             earliest - last_target_timestamp <= bind_data.cluster_gap;
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
			CheckForInterrupt(context, 0);
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
		codec_context->thread_count = bind_data.codec_threads;
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
				if ((decoded_frames_in_buffer & 255) == 0) {
					CheckForInterrupt(context, 0);
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
		if (distance > bind_data.tolerance) {
			throw InvalidInputException("No frame in LeRobot video '%s' matched timestamp %.6f within tolerance %.6f "
			                            "(closest decoded timestamp %.6f, distance %.6f)",
			                            video_path, target_timestamp, bind_data.tolerance, decoded_timestamp, distance);
		}
	}

	void ConvertFrame(const AVFrame &source, double decoded_timestamp, idx_t target_index, DecodedVideoFrame &result) {
		const auto target_width = bind_data.width > 0 ? bind_data.width : source.width;
		const auto target_height = bind_data.height > 0 ? bind_data.height : source.height;
		if (source.width <= 0 || source.height <= 0 || target_width <= 0 || target_height <= 0) {
			throw InvalidInputException("FFmpeg returned invalid dimensions for LeRobot video '%s'", video_path);
		}
		result.target_index = target_index;
		result.decoded_timestamp = decoded_timestamp;
		result.width = target_width;
		result.height = target_height;
		result.pixels = nullptr;
		if (!needs_pixels) {
			return;
		}

		const uint64_t source_byte_count =
		    static_cast<uint64_t>(source.width) * static_cast<uint64_t>(source.height) * 3;
		const uint64_t target_byte_count =
		    static_cast<uint64_t>(target_width) * static_cast<uint64_t>(target_height) * 3;
		if (source_byte_count > static_cast<uint64_t>(std::numeric_limits<idx_t>::max()) ||
		    target_byte_count > static_cast<uint64_t>(std::numeric_limits<idx_t>::max())) {
			throw OutOfMemoryException("Decoded LeRobot video frame is too large");
		}

		const auto source_format = static_cast<AVPixelFormat>(source.format);
		if (have_converted_frame && decoded_timestamp == converted_timestamp &&
		    source.width == converted_source_width && source.height == converted_source_height &&
		    source_format == converted_source_format) {
			result.pixels = &converted_pixels;
			return;
		}
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
			if (resize_source_width != source.width || resize_source_height != source.height ||
			    resize_target_width != target_width || resize_target_height != target_height) {
				resize_x_indices.resize(target_width);
				resize_y_indices.resize(target_height);
				const auto horizontal_scale =
				    static_cast<double>(static_cast<float>(source.width)) / static_cast<double>(target_width);
				const auto vertical_scale =
				    static_cast<double>(static_cast<float>(source.height)) / static_cast<double>(target_height);
				double source_x = horizontal_scale * 0.5;
				for (int32_t target_x = 0; target_x < target_width; target_x++) {
					resize_x_indices[target_x] = std::min<int32_t>(source.width - 1, static_cast<int32_t>(source_x));
					source_x += horizontal_scale;
				}
				double source_y = vertical_scale * 0.5;
				for (int32_t target_y = 0; target_y < target_height; target_y++) {
					resize_y_indices[target_y] = std::min<int32_t>(source.height - 1, static_cast<int32_t>(source_y));
					source_y += vertical_scale;
				}
				resize_source_width = source.width;
				resize_source_height = source.height;
				resize_target_width = target_width;
				resize_target_height = target_height;
			}
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
		have_converted_frame = true;
		converted_timestamp = decoded_timestamp;
		converted_source_width = source.width;
		converted_source_height = source.height;
		converted_source_format = source_format;
		result.pixels = &converted_pixels;
	}

	ClientContext &context;
	const LerobotVideoFramesBindData &bind_data;
	idx_t shard_index;
	string video_path;
	bool needs_pixels;
	const LerobotDecodeBuffer *buffer;
	DuckDBAVIOState io_state;
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
	explicit LerobotDecoderCache(idx_t max_open_shards_p) : max_open_shards(max_open_shards_p), open_count(0) {
	}

	unique_ptr<LerobotShardDecoder> Acquire(ClientContext &context, const LerobotVideoFramesBindData &bind_data,
	                                        idx_t shard_index, const string &video_path, bool needs_pixels) {
		unique_ptr<LerobotShardDecoder> stale_decoder;
		{
			lock_guard<mutex> guard(lock);
			auto entry = idle_decoders.find(shard_index);
			if (entry != idle_decoders.end()) {
				auto result = std::move(entry->second);
				idle_decoders.erase(entry);
				auto lru_entry = std::find(idle_lru.begin(), idle_lru.end(), shard_index);
				D_ASSERT(lru_entry != idle_lru.end());
				idle_lru.erase(lru_entry);
				return result;
			}

			if (open_count >= max_open_shards) {
				if (idle_lru.empty()) {
					throw InternalException("LeRobot decoder cache exhausted with no idle shard");
				}
				const auto stale_shard = idle_lru.back();
				idle_lru.pop_back();
				auto stale_entry = idle_decoders.find(stale_shard);
				D_ASSERT(stale_entry != idle_decoders.end());
				stale_decoder = std::move(stale_entry->second);
				idle_decoders.erase(stale_entry);
				open_count--;
			}
			open_count++;
		}

		// Closing an evicted remote handle and opening its replacement can perform
		// filesystem work, so neither operation holds the cache mutex.
		stale_decoder.reset();
		try {
			return make_uniq<LerobotShardDecoder>(context, bind_data, shard_index, video_path, needs_pixels);
		} catch (...) {
			lock_guard<mutex> guard(lock);
			open_count--;
			throw;
		}
	}

	void Release(idx_t shard_index, unique_ptr<LerobotShardDecoder> decoder) {
		if (!decoder) {
			return;
		}
		D_ASSERT(decoder->GetShardIndex() == shard_index);
		lock_guard<mutex> guard(lock);
		D_ASSERT(idle_decoders.find(shard_index) == idle_decoders.end());
		idle_lru.push_front(shard_index);
		idle_decoders.emplace(shard_index, std::move(decoder));
	}

	void Discard(unique_ptr<LerobotShardDecoder> decoder) {
		if (!decoder) {
			return;
		}
		{
			lock_guard<mutex> guard(lock);
			D_ASSERT(open_count > 0);
			open_count--;
		}
		decoder.reset();
	}

private:
	mutex lock;
	idx_t max_open_shards;
	idx_t open_count;
	list<idx_t> idle_lru;
	unordered_map<idx_t, unique_ptr<LerobotShardDecoder>> idle_decoders;
};

#endif

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
			max_threads = std::min<idx_t>(bind_data.decode_threads, bind_data.max_open_shards);
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
			decoder_cache = make_uniq<LerobotDecoderCache>(bind_data.max_open_shards);
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
		return decoder_cache->Acquire(context, bind_data, buffer.shard_index, bind_data.metadata->GetVideoFile(route),
		                              needs_pixels);
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
				if (current_chunk->GetValue(column, current_row).IsNull()) {
					throw InvalidInputException("LeRobot video window request columns must not contain NULL");
				}
			}
			if (current_chunk->GetValue(10, current_row).IsNull()) {
				throw InvalidInputException("LeRobot video window match count must not be NULL");
			}
			const auto match_count = current_chunk->GetValue(10, current_row).GetValue<int64_t>();
			if (match_count != 1 || current_chunk->GetValue(9, current_row).IsNull()) {
				const auto missing_episode = current_chunk->GetValue(6, current_row).GetValue<int64_t>();
				const auto missing_frame = current_chunk->GetValue(8, current_row).GetValue<int64_t>();
				if (match_count == 0) {
					throw InvalidInputException("LeRobot episode %d has no Parquet row for frame %d", missing_episode,
					                            missing_frame);
				}
				throw InvalidInputException("LeRobot episode %d has %d Parquet rows for frame %d", missing_episode,
				                            match_count, missing_frame);
			}
			request_id = current_chunk->GetValue(0, current_row).GetValue<int64_t>();
			const auto request_ordinal_value = current_chunk->GetValue(1, current_row).GetValue<int64_t>();
			const auto delta_ordinal_value = current_chunk->GetValue(2, current_row).GetValue<int64_t>();
			if (request_ordinal_value < 0 || delta_ordinal_value < 0) {
				throw InvalidInputException("Invalid LeRobot video window ordinals");
			}
			request_ordinal = static_cast<idx_t>(request_ordinal_value);
			delta_ordinal = static_cast<idx_t>(delta_ordinal_value);
			delta_timestamp = current_chunk->GetValue(3, current_row).GetValue<double>();
			delta_frame_offset = current_chunk->GetValue(4, current_row).GetValue<int64_t>();
			is_padding = current_chunk->GetValue(5, current_row).GetValue<bool>();
			episode_index = current_chunk->GetValue(6, current_row).GetValue<int64_t>();
			frame_index = current_chunk->GetValue(7, current_row).GetValue<int64_t>();
			target_frame_index = current_chunk->GetValue(8, current_row).GetValue<int64_t>();
			frame_timestamp = current_chunk->GetValue(9, current_row).GetValue<double>();
		} else {
			for (idx_t column = 0; column < 3; column++) {
				if (current_chunk->GetValue(column, current_row).IsNull()) {
					throw InvalidInputException("LeRobot frame alignment columns must not contain NULL");
				}
			}
			episode_index = current_chunk->GetValue(0, current_row).GetValue<int64_t>();
			frame_index = current_chunk->GetValue(1, current_row).GetValue<int64_t>();
			target_frame_index = frame_index;
			frame_timestamp = current_chunk->GetValue(2, current_row).GetValue<double>();
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

	void QueueTargets(const vector<LerobotDecodeTarget> &targets) {
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
#ifdef LEROBOT_HAVE_FFMPEG
	unique_ptr<LerobotDecoderCache> decoder_cache;
#endif
};

unique_ptr<GlobalTableFunctionState> LerobotVideoFramesInitGlobal(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoFramesBindData>();
	return make_uniq<LerobotVideoFramesGlobalState>(context, bind_data, input.column_ids);
}

struct LerobotVideoFramesLocalState final : public LocalTableFunctionState {
	LerobotVideoFramesLocalState() : target_position(0), have_decoded(false) {
	}

	unique_ptr<LerobotDecodeBuffer> buffer;
	idx_t target_position;
#ifdef LEROBOT_HAVE_FFMPEG
	unique_ptr<LerobotShardDecoder> decoder;
#endif
	DecodedVideoFrame decoded;
	bool have_decoded;
};

unique_ptr<LocalTableFunctionState> LerobotVideoFramesInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                                GlobalTableFunctionState *) {
	return make_uniq<LerobotVideoFramesLocalState>();
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
		WriteInt32(output, row, 3);
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
		WriteInt32(output, row, 3);
		break;
	case LEROBOT_WINDOW_IMAGE:
		D_ASSERT(decoded && decoded->pixels);
		WriteBlob(output, row, *decoded->pixels);
		break;
	default:
		throw InternalException("Invalid lerobot_video_windows projected column");
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

void LerobotVideoFramesFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
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
	SetOutputCardinality(output, count, 0);
}

void AddVideoDecodeNamedParameters(TableFunction &function) {
	function.named_parameters["video_keys"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["width"] = LogicalType::BIGINT;
	function.named_parameters["height"] = LogicalType::BIGINT;
	function.named_parameters["tolerance"] = LogicalType::DOUBLE;
	function.named_parameters["cluster_gap"] = LogicalType::DOUBLE;
	function.named_parameters["batch_size"] = LogicalType::BIGINT;
	function.named_parameters["target_buffer_size"] = LogicalType::BIGINT;
	function.named_parameters["max_open_shards"] = LogicalType::BIGINT;
	function.named_parameters["decode_threads"] = LogicalType::BIGINT;
	function.named_parameters["max_pending_targets"] = LogicalType::BIGINT;
	function.named_parameters["max_output_bytes"] = LogicalType::BIGINT;
	function.named_parameters["codec_threads"] = LogicalType::BIGINT;
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	function.projection_pushdown = true;
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

} // namespace duckdb
