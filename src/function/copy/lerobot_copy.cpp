#include "function/lerobot_copy.hpp"
#include "function/lerobot_video_writer.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/copy_function_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>

namespace duckdb {

namespace {

static const idx_t LEROBOT_DEFAULT_CHUNK_SIZE = 1000;
static const double LEROBOT_DEFAULT_DATA_FILE_SIZE_MB = 100;
static const double LEROBOT_DEFAULT_VIDEO_FILE_SIZE_MB = 200;
static const idx_t LEROBOT_DEFAULT_METADATA_BUFFER_SIZE = 10;
static const idx_t LEROBOT_DEFAULT_MAX_VISUAL_FRAME_BYTES = 64ULL * 1024ULL * 1024ULL;
static const idx_t LEROBOT_QUANTILE_BIN_COUNT = 5000;
static const idx_t LEROBOT_STATS_DIMENSION_BATCH_SIZE = 64;
static const char *LEROBOT_CODEBASE_VERSION = "v3.0";
static const char *LEROBOT_DATA_PATH = "data/chunk-{chunk_index:03d}/file-{file_index:03d}.parquet";
static const char *LEROBOT_VIDEO_PATH = "videos/{video_key}/chunk-{chunk_index:03d}/file-{file_index:03d}.mp4";

struct LerobotFeature {
	string name;
	string dtype;
	string json;
	string names_json;
	vector<idx_t> shape;
	LogicalType storage_type;
	LogicalType output_type;
	idx_t input_index = DConstants::INVALID_INDEX;
	idx_t output_index = DConstants::INVALID_INDEX;
	bool user_defined = true;
	bool is_string = false;
	bool is_image = false;
	bool is_video = false;
	bool is_depth = false;

	bool HasStatistics() const {
		return !is_string;
	}
};

struct LerobotFeatureStats {
	vector<double> min;
	vector<double> max;
	vector<double> mean;
	vector<double> stddev;
	vector<double> q01;
	vector<double> q10;
	vector<double> q50;
	vector<double> q90;
	vector<double> q99;
	bool q01_is_float32 = false;
	bool q10_is_float32 = false;
	bool q50_is_float32 = false;
	bool q90_is_float32 = false;
	bool q99_is_float32 = false;
	bool stddev_is_float32 = false;
	int64_t count = 0;
};

string JsonEscape(const string &input) {
	std::ostringstream result;
	result << '"';
	for (const auto ch : input) {
		switch (ch) {
		case '"':
			result << "\\\"";
			break;
		case '\\':
			result << "\\\\";
			break;
		case '\b':
			result << "\\b";
			break;
		case '\f':
			result << "\\f";
			break;
		case '\n':
			result << "\\n";
			break;
		case '\r':
			result << "\\r";
			break;
		case '\t':
			result << "\\t";
			break;
		default:
			if (static_cast<unsigned char>(ch) < 0x20) {
				result << "\\u" << std::hex << std::setw(4) << std::setfill('0')
				       << static_cast<int>(static_cast<unsigned char>(ch)) << std::dec;
			} else {
				result << ch;
			}
		}
	}
	result << '"';
	return result.str();
}

string JsonNumber(double value) {
	if (!std::isfinite(value)) {
		throw InvalidInputException("LeRobot statistics cannot contain NaN or infinity");
	}
	std::ostringstream result;
	result << std::setprecision(17) << value;
	return result.str();
}

string PadIndex(idx_t value) {
	std::ostringstream result;
	result << std::setw(3) << std::setfill('0') << value;
	return result.str();
}

string DataRelativePath(idx_t chunk_index, idx_t file_index) {
	return "data/chunk-" + PadIndex(chunk_index) + "/file-" + PadIndex(file_index) + ".parquet";
}

string EpisodesRelativePath(idx_t chunk_index, idx_t file_index) {
	return "meta/episodes/chunk-" + PadIndex(chunk_index) + "/file-" + PadIndex(file_index) + ".parquet";
}

string VideoRelativePath(const string &video_key, idx_t chunk_index, idx_t file_index) {
	return "videos/" + video_key + "/chunk-" + PadIndex(chunk_index) + "/file-" + PadIndex(file_index) + ".mp4";
}

string FrameFileName(idx_t frame_index, bool depth) {
	std::ostringstream result;
	result << "frame-" << std::setw(6) << std::setfill('0') << frame_index << (depth ? ".tiff" : ".png");
	return result.str();
}

void AdvanceFileIndex(idx_t &chunk_index, idx_t &file_index, idx_t chunk_size) {
	if (file_index == chunk_size - 1) {
		file_index = 0;
		chunk_index++;
	} else {
		file_index++;
	}
}

idx_t ShapeWidth(const vector<idx_t> &shape) {
	idx_t width = 1;
	for (const auto dimension : shape) {
		if (dimension == 0 || width > NumericLimits<idx_t>::Maximum() / dimension) {
			throw BinderException("LeRobot feature shape is empty or too large");
		}
		width *= dimension;
	}
	return width;
}

LogicalType DtypeToLogicalType(const string &dtype) {
	if (dtype == "float32") {
		return LogicalType::FLOAT;
	}
	if (dtype == "float64") {
		return LogicalType::DOUBLE;
	}
	if (dtype == "int8") {
		return LogicalType::TINYINT;
	}
	if (dtype == "int16") {
		return LogicalType::SMALLINT;
	}
	if (dtype == "int32") {
		return LogicalType::INTEGER;
	}
	if (dtype == "int64") {
		return LogicalType::BIGINT;
	}
	if (dtype == "uint8") {
		return LogicalType::UTINYINT;
	}
	if (dtype == "uint16") {
		return LogicalType::USMALLINT;
	}
	if (dtype == "uint32") {
		return LogicalType::UINTEGER;
	}
	if (dtype == "uint64") {
		return LogicalType::UBIGINT;
	}
	if (dtype == "bool") {
		return LogicalType::BOOLEAN;
	}
	if (dtype == "string") {
		return LogicalType::VARCHAR;
	}
	throw BinderException("Unsupported LeRobot v3 feature dtype '%s'", dtype);
}

LogicalType FeatureStorageType(const string &dtype, const vector<idx_t> &shape) {
	if (dtype == "image" || dtype == "video") {
		return LogicalType::BLOB;
	}
	auto result = DtypeToLogicalType(dtype);
	if (shape.size() == 1 && shape[0] == 1) {
		return result;
	}
	for (auto entry = shape.rbegin(); entry != shape.rend(); ++entry) {
		result = LogicalType::ARRAY(result, *entry);
	}
	return result;
}

LogicalType FeatureOutputType(const string &dtype, const vector<idx_t> &shape) {
	if (dtype == "image") {
		return LogicalType::STRUCT({{"bytes", LogicalType::BLOB}, {"path", LogicalType::VARCHAR}});
	}
	return FeatureStorageType(dtype, shape);
}

LogicalType NestedListType(const LogicalType &leaf, idx_t dimensions) {
	auto result = leaf;
	for (idx_t index = 0; index < dimensions; index++) {
		result = LogicalType::LIST(result);
	}
	return result;
}

void ThrowQueryError(const char *description, const QueryResult &result) {
	throw BinderException("Failed to bind LeRobot %s: %s", description, result.GetError());
}

vector<LerobotFeature> ParseUserFeatures(ClientContext &context, const string &features_json) {
	Connection connection(*context.db);
	auto result = connection.Query("SELECT CAST(key AS VARCHAR), json_extract_string(value, '$.dtype'), "
	                               "CAST(json_extract(value, '$.shape') AS BIGINT[]), CAST(value AS VARCHAR), "
	                               "COALESCE(TRY_CAST(json_extract(value, '$.info.is_depth_map') AS BOOLEAN), false), "
	                               "COALESCE(CAST(json_extract(value, '$.names') AS VARCHAR), 'null') "
	                               "FROM json_each(json(" +
	                               Value(features_json).ToSQLString() + ")) ORDER BY id");
	if (result->HasError()) {
		ThrowQueryError("FEATURES JSON", *result);
	}

	vector<LerobotFeature> features;
	case_insensitive_set_t names;
	while (true) {
		auto chunk = result->Fetch();
		if (!chunk) {
			break;
		}
		for (idx_t row = 0; row < chunk->size(); row++) {
			for (idx_t column = 0; column < 4; column++) {
				if (chunk->GetValue(column, row).IsNull()) {
					throw BinderException("Each LeRobot feature requires non-NULL dtype and shape fields");
				}
			}
			LerobotFeature feature;
			feature.name = StringValue::Get(chunk->GetValue(0, row));
			feature.dtype = StringValue::Get(chunk->GetValue(1, row));
			feature.json = StringValue::Get(chunk->GetValue(3, row));
			feature.is_depth = BooleanValue::Get(chunk->GetValue(4, row));
			feature.names_json = StringValue::Get(chunk->GetValue(5, row));
			auto shape_value = chunk->GetValue(2, row);
			for (const auto &dimension : ListValue::GetChildren(shape_value)) {
				if (dimension.IsNull()) {
					throw BinderException("LeRobot feature '%s' shape cannot contain NULL", feature.name);
				}
				auto value = dimension.GetValue<int64_t>();
				if (value <= 0) {
					throw BinderException("LeRobot feature '%s' shape dimensions must be positive", feature.name);
				}
				if (static_cast<uint64_t>(value) > NumericLimits<idx_t>::Maximum()) {
					throw BinderException("LeRobot feature '%s' shape dimension is too large", feature.name);
				}
				feature.shape.push_back(static_cast<idx_t>(value));
			}
			if (feature.name.empty()) {
				throw BinderException("LeRobot feature names cannot be empty");
			}
			if (feature.name.find('/') != string::npos) {
				throw BinderException("LeRobot feature names must not contain '/': '%s'", feature.name);
			}
			if (feature.shape.empty() || feature.shape.size() > 5) {
				throw BinderException("LeRobot feature '%s' requires a shape with one to five dimensions",
				                      feature.name);
			}
			if (feature.name == "task" || feature.name == "timestamp" || feature.name == "frame_index" ||
			    feature.name == "episode_index" || feature.name == "index" || feature.name == "task_index") {
				throw BinderException("LeRobot feature '%s' is generated by FORMAT lerobot and must not be in FEATURES",
				                      feature.name);
			}
			if (!names.insert(feature.name).second) {
				throw BinderException("Duplicate LeRobot feature name '%s'", feature.name);
			}
			feature.is_string = feature.dtype == "string";
			feature.is_image = feature.dtype == "image";
			feature.is_video = feature.dtype == "video";
			if ((feature.is_image || feature.is_video) && feature.shape.size() != 3) {
				throw BinderException("LeRobot visual feature '%s' requires a three-dimensional HWC shape",
				                      feature.name);
			}
			if (feature.is_depth && !feature.is_image && !feature.is_video) {
				throw BinderException("LeRobot feature '%s' marks is_depth_map but is not visual", feature.name);
			}
			if ((feature.is_image || feature.is_video) &&
			    feature.shape[2] != static_cast<idx_t>(feature.is_depth ? 1 : 3)) {
				throw BinderException("LeRobot visual feature '%s' requires %d HWC channel(s)", feature.name,
				                      feature.is_depth ? 1 : 3);
			}
			feature.storage_type = FeatureStorageType(feature.dtype, feature.shape);
			feature.output_type = FeatureOutputType(feature.dtype, feature.shape);
			ShapeWidth(feature.shape);
			features.push_back(std::move(feature));
		}
	}
	return features;
}

LerobotFeature DefaultFeature(const string &name, const string &dtype) {
	LerobotFeature feature;
	feature.name = name;
	feature.dtype = dtype;
	feature.shape = {1};
	feature.storage_type = DtypeToLogicalType(dtype);
	feature.output_type = feature.storage_type;
	feature.user_defined = false;
	feature.names_json = "null";
	feature.json = "{\"dtype\":" + JsonEscape(dtype) + ",\"shape\":[1],\"names\":null}";
	return feature;
}

string FullFeaturesJSON(const vector<LerobotFeature> &features) {
	string result = "{";
	for (idx_t index = 0; index < features.size(); index++) {
		if (index > 0) {
			result += ',';
		}
		result += JsonEscape(features[index].name) + ':' + features[index].json;
	}
	result += '}';
	return result;
}

string ShapeJSON(const vector<idx_t> &shape) {
	string result = "[";
	for (idx_t index = 0; index < shape.size(); index++) {
		if (index > 0) {
			result += ',';
		}
		result += std::to_string(shape[index]);
	}
	return result + ']';
}

string HuggingFaceFeatureJSON(const LerobotFeature &feature) {
	if (feature.is_video) {
		return string();
	}
	if (feature.is_image) {
		return "{\"_type\":\"Image\"}";
	}
	if (feature.shape.size() == 1 && feature.shape[0] == 1) {
		return "{\"dtype\":" + JsonEscape(feature.dtype) + ",\"_type\":\"Value\"}";
	}
	if (feature.shape.size() == 1) {
		return "{\"feature\":{\"dtype\":" + JsonEscape(feature.dtype) +
		       ",\"_type\":\"Value\"},\"length\":" + std::to_string(feature.shape[0]) + ",\"_type\":\"List\"}";
	}
	return "{\"shape\":" +
	       [&]() {
		       string shape = "[";
		       for (idx_t index = 0; index < feature.shape.size(); index++) {
			       if (index > 0) {
				       shape += ',';
			       }
			       shape += std::to_string(feature.shape[index]);
		       }
		       return shape + ']';
	       }() +
	       ",\"dtype\":" + JsonEscape(feature.dtype) + ",\"_type\":\"Array" + std::to_string(feature.shape.size()) +
	       "D\"}";
}

string HuggingFaceMetadataJSON(const vector<LerobotFeature> &features) {
	string result = "{\"info\":{\"features\":{";
	bool first = true;
	for (const auto &feature : features) {
		auto json = HuggingFaceFeatureJSON(feature);
		if (json.empty()) {
			continue;
		}
		if (!first) {
			result += ',';
		}
		first = false;
		result += JsonEscape(feature.name) + ':' + json;
	}
	return result + "}}}";
}

string TasksPandasMetadataJSON() {
	return "{\"index_columns\":[\"task\"],\"column_indexes\":[{\"name\":null,\"field_name\":null,"
	       "\"pandas_type\":\"unicode\",\"numpy_type\":\"object\",\"metadata\":{\"encoding\":\"UTF-8\"}}],"
	       "\"columns\":[{\"name\":\"task_index\",\"field_name\":\"task_index\",\"pandas_type\":\"int64\","
	       "\"numpy_type\":\"int64\",\"metadata\":null},{\"name\":\"task\",\"field_name\":\"task\","
	       "\"pandas_type\":\"unicode\",\"numpy_type\":\"object\",\"metadata\":null}],\"attributes\":{},"
	       "\"creator\":{\"library\":\"pyarrow\",\"version\":\"25.0.1\"},\"pandas_version\":\"2.3.3\"}";
}

CopyFunction GetParquetCopyFunction(ClientContext &context) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<CopyFunctionCatalogEntry>(context, DEFAULT_SCHEMA, "parquet");
	return entry.function;
}

unique_ptr<FunctionData> BindParquet(ClientContext &context, const CopyFunction &function, const vector<string> &names,
                                     const vector<LogicalType> &types, const string &metadata_key = string(),
                                     const string &metadata_value = string()) {
	CopyInfo info;
	info.format = "parquet";
	info.is_format_auto_detected = false;
	info.options["compression"] = {Value("snappy")};
	if (!metadata_key.empty()) {
		child_list_t<Value> entries;
		entries.emplace_back(metadata_key, Value(metadata_value));
		info.options["kv_metadata"] = {Value::STRUCT(std::move(entries))};
	}
	CopyFunctionBindInput input(info, function.function_info);
	return function.copy_to_bind(context, input, names, types);
}

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

struct LerobotCopyBindData : public FunctionData {
	LerobotCopyBindData() : parquet_function("parquet") {
	}

	vector<string> input_names;
	vector<LogicalType> input_types;
	vector<LerobotFeature> features;
	vector<string> data_names;
	vector<LogicalType> data_types;
	vector<string> episode_names;
	vector<LogicalType> episode_types;
	CopyFunction parquet_function;
	idx_t episode_input_index = DConstants::INVALID_INDEX;
	idx_t task_input_index = DConstants::INVALID_INDEX;
	idx_t fps = 0;
	idx_t chunks_size = LEROBOT_DEFAULT_CHUNK_SIZE;
	double data_file_size_mb = LEROBOT_DEFAULT_DATA_FILE_SIZE_MB;
	double video_file_size_mb = LEROBOT_DEFAULT_VIDEO_FILE_SIZE_MB;
	idx_t metadata_buffer_size = LEROBOT_DEFAULT_METADATA_BUFFER_SIZE;
	idx_t max_visual_frame_bytes = LEROBOT_DEFAULT_MAX_VISUAL_FRAME_BYTES;
	optional_idx encoder_threads;
	string robot_type;
	bool has_robot_type = false;
	string features_json;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<LerobotCopyBindData>();
		result->input_names = input_names;
		result->input_types = input_types;
		result->features = features;
		result->data_names = data_names;
		result->data_types = data_types;
		result->episode_names = episode_names;
		result->episode_types = episode_types;
		result->parquet_function = parquet_function;
		result->episode_input_index = episode_input_index;
		result->task_input_index = task_input_index;
		result->fps = fps;
		result->chunks_size = chunks_size;
		result->data_file_size_mb = data_file_size_mb;
		result->video_file_size_mb = video_file_size_mb;
		result->metadata_buffer_size = metadata_buffer_size;
		result->max_visual_frame_bytes = max_visual_frame_bytes;
		result->encoder_threads = encoder_threads;
		result->robot_type = robot_type;
		result->has_robot_type = has_robot_type;
		result->features_json = features_json;
		return std::move(result);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<LerobotCopyBindData>();
		return input_names == other.input_names && input_types == other.input_types && fps == other.fps &&
		       features_json == other.features_json && chunks_size == other.chunks_size &&
		       data_file_size_mb == other.data_file_size_mb && video_file_size_mb == other.video_file_size_mb &&
		       metadata_buffer_size == other.metadata_buffer_size &&
		       max_visual_frame_bytes == other.max_visual_frame_bytes && encoder_threads == other.encoder_threads &&
		       has_robot_type == other.has_robot_type && robot_type == other.robot_type;
	}
};

void AppendStatColumns(LerobotCopyBindData &bind, const LerobotFeature &feature) {
	if (!feature.HasStatistics()) {
		return;
	}
	vector<idx_t> stat_shape = feature.shape;
	if (feature.is_image || feature.is_video) {
		stat_shape = {feature.shape[2], 1, 1};
	}
	auto value_type = NestedListType(LogicalType::DOUBLE, stat_shape.size());
	auto count_type = LogicalType::LIST(LogicalType::BIGINT);
	static const char *names[] = {"min", "max", "mean", "std", "count", "q01", "q10", "q50", "q90", "q99"};
	for (const auto stat_name : names) {
		bind.episode_names.push_back("stats/" + feature.name + "/" + stat_name);
		bind.episode_types.push_back(string(stat_name) == "count" ? count_type : value_type);
	}
}

unique_ptr<FunctionData> LerobotCopyBind(ClientContext &context, CopyFunctionBindInput &input,
                                         const vector<string> &names, const vector<LogicalType> &types) {
	if (names.size() != types.size()) {
		throw InternalException("LeRobot COPY input names/types size mismatch");
	}
	auto result = make_uniq<LerobotCopyBindData>();
	for (const auto &name : names) {
		result->input_names.push_back(name);
	}
	result->input_types = types;

	const auto &fps_value = GetSingleOption(input, "fps", true);
	auto fps = fps_value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
	if (fps <= 0) {
		throw BinderException("LeRobot FPS must be positive");
	}
	if (static_cast<uint64_t>(fps) > NumericLimits<idx_t>::Maximum()) {
		throw BinderException("LeRobot FPS is too large");
	}
	result->fps = static_cast<idx_t>(fps);
	const auto &features_value = GetSingleOption(input, "features", true);
	result->features_json = StringValue::Get(features_value.DefaultCastAs(LogicalType::VARCHAR));
	result->features = ParseUserFeatures(context, result->features_json);
	result->features.push_back(DefaultFeature("timestamp", "float32"));
	result->features.push_back(DefaultFeature("frame_index", "int64"));
	result->features.push_back(DefaultFeature("episode_index", "int64"));
	result->features.push_back(DefaultFeature("index", "int64"));
	result->features.push_back(DefaultFeature("task_index", "int64"));
	result->features_json = FullFeaturesJSON(result->features);

	auto chunks_size = GetNumericOption<double>(input, "chunks_size", LEROBOT_DEFAULT_CHUNK_SIZE);
	auto metadata_buffer_size =
	    GetNumericOption<double>(input, "metadata_buffer_size", LEROBOT_DEFAULT_METADATA_BUFFER_SIZE);
	result->data_file_size_mb =
	    GetNumericOption<double>(input, "data_files_size_in_mb", LEROBOT_DEFAULT_DATA_FILE_SIZE_MB);
	result->video_file_size_mb =
	    GetNumericOption<double>(input, "video_files_size_in_mb", LEROBOT_DEFAULT_VIDEO_FILE_SIZE_MB);
	if (chunks_size <= 0 || std::floor(chunks_size) != chunks_size) {
		throw BinderException("LeRobot CHUNKS_SIZE must be a positive integer");
	}
	if (metadata_buffer_size <= 0 || std::floor(metadata_buffer_size) != metadata_buffer_size) {
		throw BinderException("LeRobot METADATA_BUFFER_SIZE must be a positive integer");
	}
	if (!std::isfinite(result->data_file_size_mb) || !std::isfinite(result->video_file_size_mb) ||
	    !(result->data_file_size_mb > 0) || !(result->video_file_size_mb > 0)) {
		throw BinderException("LeRobot data and video file size limits must be positive");
	}
	if (chunks_size > static_cast<double>(NumericLimits<idx_t>::Maximum()) ||
	    metadata_buffer_size > static_cast<double>(NumericLimits<idx_t>::Maximum())) {
		throw BinderException("LeRobot chunk or metadata buffer size is too large");
	}
	result->chunks_size = static_cast<idx_t>(chunks_size);
	result->metadata_buffer_size = static_cast<idx_t>(metadata_buffer_size);
	const auto &max_visual_frame_bytes = GetSingleOption(input, "max_visual_frame_bytes", false);
	if (!max_visual_frame_bytes.IsNull()) {
		auto value = max_visual_frame_bytes.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
		if (value == 0 || value > NumericLimits<idx_t>::Maximum()) {
			throw BinderException("LeRobot MAX_VISUAL_FRAME_BYTES must be a positive integer");
		}
		result->max_visual_frame_bytes = static_cast<idx_t>(value);
	}
	const auto &encoder_threads = GetSingleOption(input, "encoder_threads", false);
	if (!encoder_threads.IsNull()) {
		auto value = encoder_threads.DefaultCastAs(LogicalType::UBIGINT).GetValue<uint64_t>();
		if (value == 0 || value >= NumericLimits<idx_t>::Maximum()) {
			throw BinderException("LeRobot ENCODER_THREADS must be a positive integer");
		}
		result->encoder_threads = optional_idx(static_cast<idx_t>(value));
	}
	const auto &robot_type = GetSingleOption(input, "robot_type", false);
	if (!robot_type.IsNull()) {
		result->robot_type = StringValue::Get(robot_type.DefaultCastAs(LogicalType::VARCHAR));
		result->has_robot_type = true;
	}

	case_insensitive_map_t<idx_t> input_indexes;
	for (idx_t index = 0; index < result->input_names.size(); index++) {
		if (!input_indexes.emplace(result->input_names[index], index).second) {
			throw BinderException("FORMAT lerobot input columns must be unique");
		}
	}
	auto episode_entry = input_indexes.find("episode_index");
	auto task_entry = input_indexes.find("task");
	if (episode_entry == input_indexes.end() || task_entry == input_indexes.end()) {
		throw BinderException("FORMAT lerobot input requires episode_index and task columns");
	}
	result->episode_input_index = episode_entry->second;
	result->task_input_index = task_entry->second;
	if (types[result->episode_input_index] != LogicalType::BIGINT) {
		throw BinderException("FORMAT lerobot episode_index must be BIGINT, found %s",
		                      types[result->episode_input_index].ToString());
	}
	if (types[result->task_input_index] != LogicalType::VARCHAR) {
		throw BinderException("FORMAT lerobot task must be VARCHAR, found %s",
		                      types[result->task_input_index].ToString());
	}

	idx_t expected_columns = 2;
	for (auto &feature : result->features) {
		if (feature.user_defined) {
			auto entry = input_indexes.find(feature.name);
			if (entry == input_indexes.end()) {
				throw BinderException("FORMAT lerobot input is missing feature column '%s'", feature.name);
			}
			feature.input_index = entry->second;
			if (types[feature.input_index] != feature.storage_type) {
				throw BinderException("LeRobot feature '%s' requires DuckDB type %s, found %s", feature.name,
				                      feature.storage_type.ToString(), types[feature.input_index].ToString());
			}
			expected_columns++;
		}
		if (!feature.is_video) {
			feature.output_index = result->data_names.size();
			result->data_names.push_back(feature.name);
			result->data_types.push_back(feature.output_type);
		}
	}
	if (result->input_names.size() != expected_columns) {
		throw BinderException("FORMAT lerobot input has %llu columns; expected exactly episode_index, task, and %llu "
		                      "user feature columns",
		                      result->input_names.size(), expected_columns - 2);
	}

	result->episode_names = {"episode_index",      "tasks",           "length", "data/chunk_index", "data/file_index",
	                         "dataset_from_index", "dataset_to_index"};
	result->episode_types = {LogicalType::BIGINT, LogicalType::LIST(LogicalType::VARCHAR),
	                         LogicalType::BIGINT, LogicalType::BIGINT,
	                         LogicalType::BIGINT, LogicalType::BIGINT,
	                         LogicalType::BIGINT};
	for (const auto &feature : result->features) {
		if (!feature.is_video) {
			continue;
		}
		const auto prefix = "videos/" + feature.name + "/";
		result->episode_names.push_back(prefix + "chunk_index");
		result->episode_names.push_back(prefix + "file_index");
		result->episode_names.push_back(prefix + "from_timestamp");
		result->episode_names.push_back(prefix + "to_timestamp");
		result->episode_types.push_back(LogicalType::BIGINT);
		result->episode_types.push_back(LogicalType::BIGINT);
		result->episode_types.push_back(LogicalType::DOUBLE);
		result->episode_types.push_back(LogicalType::DOUBLE);
	}
	for (const auto &feature : result->features) {
		AppendStatColumns(*result, feature);
	}
	result->episode_names.push_back("meta/episodes/chunk_index");
	result->episode_names.push_back("meta/episodes/file_index");
	result->episode_types.push_back(LogicalType::BIGINT);
	result->episode_types.push_back(LogicalType::BIGINT);

	result->parquet_function = GetParquetCopyFunction(context);
	return std::move(result);
}

void LerobotCopyOptions(ClientContext &, CopyOptionsInput &input) {
	input.options["fps"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["features"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	input.options["robot_type"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	input.options["chunks_size"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["data_files_size_in_mb"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	input.options["video_files_size_in_mb"] = CopyOption(LogicalType::DOUBLE, CopyOptionMode::WRITE_ONLY);
	input.options["metadata_buffer_size"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["max_visual_frame_bytes"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	input.options["encoder_threads"] = CopyOption(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
}

struct LerobotCopyLocalData : public LocalFunctionData {};

struct DelegatedParquetWriter {
	DelegatedParquetWriter(const CopyFunction &function_p, unique_ptr<FunctionData> bind_data_p)
	    : function(function_p), bind_data(std::move(bind_data_p)) {
	}

	void Open(ClientContext &context, const string &path_p) {
		if (global_data) {
			throw InternalException("LeRobot delegated Parquet writer is already open");
		}
		path = path_p;
		global_data = function.copy_to_initialize_global(context, *bind_data, path);
	}

	void Flush(ClientContext &context, unique_ptr<ColumnDataCollection> collection) {
		if (!global_data || !collection || collection->Count() == 0) {
			throw InternalException("Cannot flush an empty or unopened LeRobot Parquet writer");
		}
		auto prepared = function.prepare_batch(context, *bind_data, *global_data, std::move(collection));
		function.flush_batch(context, *bind_data, *global_data, *prepared);
	}

	bool SizeAtLeast(double bytes) const {
		if (!global_data) {
			return false;
		}
		if (bytes <= 0) {
			return true;
		}
		if (!std::isfinite(bytes) || bytes > static_cast<double>(NumericLimits<idx_t>::Maximum())) {
			return false;
		}
		if (!function.rotate_next_file) {
			throw InternalException("DuckDB 1.5 Parquet COPY is missing rotate_next_file");
		}
		const auto minimum_size = static_cast<idx_t>(std::ceil(bytes));
		const auto maximum_smaller_size = minimum_size == 0 ? 0 : minimum_size - 1;
		return function.rotate_next_file(*global_data, *bind_data, optional_idx(maximum_smaller_size));
	}

	void Close(ClientContext &context) {
		if (!global_data) {
			return;
		}
		function.copy_to_finalize(context, *bind_data, *global_data);
		global_data.reset();
	}

	CopyFunction function;
	unique_ptr<FunctionData> bind_data;
	unique_ptr<GlobalFunctionData> global_data;
	string path;
};

bool UsesFloat32Statistics(const LerobotFeature &feature) {
	// The native writer constructs timestamps from Python division before the
	// Arrow schema casts them to float32, so episode statistics see float64.
	if (!feature.user_defined && feature.name == "timestamp") {
		return false;
	}
	return feature.is_image || feature.is_video || feature.dtype == "float32" || feature.dtype == "int8" ||
	       feature.dtype == "int16" || feature.dtype == "uint8" || feature.dtype == "uint16" || feature.dtype == "bool";
}

struct LerobotStatsRowReader {
	virtual ~LerobotStatsRowReader() {
	}

	virtual idx_t Count() const = 0;
	virtual void Next(vector<double> &row) = 0;
};

using LerobotStatsReaderFactory = std::function<unique_ptr<LerobotStatsRowReader>()>;

template <class T>
struct LerobotReductionTotals {
	explicit LerobotReductionTotals(idx_t width) : sum(width, static_cast<T>(0)), square_sum(width, static_cast<T>(0)) {
	}

	vector<T> sum;
	vector<T> square_sum;
};

void ReadStatsRow(LerobotStatsRowReader &reader, idx_t width, vector<double> &row) {
	row.clear();
	reader.Next(row);
	if (row.size() != width) {
		throw InternalException("LeRobot statistics width changed from %llu to %llu", width, row.size());
	}
}

template <class T>
T PrepareReductionValue(double raw_value, idx_t dimension, vector<double> &minimum, vector<double> &maximum) {
	const auto value = static_cast<T>(raw_value);
	if (!std::isfinite(value)) {
		throw InvalidInputException("LeRobot numeric features cannot contain NaN or infinity");
	}
	minimum[dimension] = MinValue(minimum[dimension], static_cast<double>(value));
	maximum[dimension] = MaxValue(maximum[dimension], static_cast<double>(value));
	return value;
}

template <class T>
LerobotReductionTotals<T> NumpyPairwiseStats(LerobotStatsRowReader &reader, idx_t row_width, idx_t dimension_offset,
                                             idx_t width, idx_t count, vector<double> &minimum, vector<double> &maximum,
                                             vector<double> &row) {
	static const idx_t NUMPY_PAIRWISE_BLOCK_SIZE = 128;
	LerobotReductionTotals<T> result(width);
	if (count < 8) {
		for (idx_t index = 0; index < count; index++) {
			ReadStatsRow(reader, row_width, row);
			for (idx_t dimension = 0; dimension < width; dimension++) {
				const auto value =
				    PrepareReductionValue<T>(row[dimension_offset + dimension], dimension, minimum, maximum);
				volatile T product = value * value;
				volatile T updated_sum = result.sum[dimension] + value;
				volatile T updated_square_sum = result.square_sum[dimension] + product;
				result.sum[dimension] = updated_sum;
				result.square_sum[dimension] = updated_square_sum;
			}
		}
		return result;
	}
	if (count <= NUMPY_PAIRWISE_BLOCK_SIZE) {
		vector<vector<T>> accumulators(8, vector<T>(width));
		vector<vector<T>> square_accumulators(8, vector<T>(width));
		for (idx_t accumulator = 0; accumulator < 8; accumulator++) {
			ReadStatsRow(reader, row_width, row);
			for (idx_t dimension = 0; dimension < width; dimension++) {
				const auto value =
				    PrepareReductionValue<T>(row[dimension_offset + dimension], dimension, minimum, maximum);
				volatile T product = value * value;
				accumulators[accumulator][dimension] = value;
				square_accumulators[accumulator][dimension] = product;
			}
		}
		idx_t index;
		for (index = 8; index < count - (count % 8); index += 8) {
			for (idx_t accumulator = 0; accumulator < 8; accumulator++) {
				ReadStatsRow(reader, row_width, row);
				for (idx_t dimension = 0; dimension < width; dimension++) {
					const auto value =
					    PrepareReductionValue<T>(row[dimension_offset + dimension], dimension, minimum, maximum);
					volatile T product = value * value;
					volatile T updated_sum = accumulators[accumulator][dimension] + value;
					volatile T updated_square_sum = square_accumulators[accumulator][dimension] + product;
					accumulators[accumulator][dimension] = updated_sum;
					square_accumulators[accumulator][dimension] = updated_square_sum;
				}
			}
		}
		for (idx_t dimension = 0; dimension < width; dimension++) {
			// This is NumPy's exact eight-way collapse order from loops_utils.h.src.
			volatile T sum_01 = accumulators[0][dimension] + accumulators[1][dimension];
			volatile T sum_23 = accumulators[2][dimension] + accumulators[3][dimension];
			volatile T sum_45 = accumulators[4][dimension] + accumulators[5][dimension];
			volatile T sum_67 = accumulators[6][dimension] + accumulators[7][dimension];
			volatile T sum_0123 = sum_01 + sum_23;
			volatile T sum_4567 = sum_45 + sum_67;
			volatile T sum = sum_0123 + sum_4567;
			result.sum[dimension] = sum;

			volatile T square_sum_01 = square_accumulators[0][dimension] + square_accumulators[1][dimension];
			volatile T square_sum_23 = square_accumulators[2][dimension] + square_accumulators[3][dimension];
			volatile T square_sum_45 = square_accumulators[4][dimension] + square_accumulators[5][dimension];
			volatile T square_sum_67 = square_accumulators[6][dimension] + square_accumulators[7][dimension];
			volatile T square_sum_0123 = square_sum_01 + square_sum_23;
			volatile T square_sum_4567 = square_sum_45 + square_sum_67;
			volatile T square_sum = square_sum_0123 + square_sum_4567;
			result.square_sum[dimension] = square_sum;
		}
		for (; index < count; index++) {
			ReadStatsRow(reader, row_width, row);
			for (idx_t dimension = 0; dimension < width; dimension++) {
				const auto value =
				    PrepareReductionValue<T>(row[dimension_offset + dimension], dimension, minimum, maximum);
				volatile T product = value * value;
				volatile T updated_sum = result.sum[dimension] + value;
				volatile T updated_square_sum = result.square_sum[dimension] + product;
				result.sum[dimension] = updated_sum;
				result.square_sum[dimension] = updated_square_sum;
			}
		}
		return result;
	}

	auto left_count = count / 2;
	left_count -= left_count % 8;
	auto left = NumpyPairwiseStats<T>(reader, row_width, dimension_offset, width, left_count, minimum, maximum, row);
	auto right =
	    NumpyPairwiseStats<T>(reader, row_width, dimension_offset, width, count - left_count, minimum, maximum, row);
	for (idx_t dimension = 0; dimension < width; dimension++) {
		volatile T sum = left.sum[dimension] + right.sum[dimension];
		volatile T square_sum = left.square_sum[dimension] + right.square_sum[dimension];
		result.sum[dimension] = sum;
		result.square_sum[dimension] = square_sum;
	}
	return result;
}

template <class T>
struct LerobotHistogram {
	LerobotHistogram(idx_t width, const vector<double> &minimum, const vector<double> &maximum)
	    : counts(width, vector<int64_t>(LEROBOT_QUANTILE_BIN_COUNT, 0)),
	      edges(width, vector<T>(LEROBOT_QUANTILE_BIN_COUNT + 1)) {
		for (idx_t dimension = 0; dimension < width; dimension++) {
			const auto low = static_cast<T>(minimum[dimension]) - static_cast<T>(1e-10);
			const auto high = static_cast<T>(maximum[dimension]) + static_cast<T>(1e-10);
			const auto step = (high - low) / static_cast<T>(LEROBOT_QUANTILE_BIN_COUNT);
			for (idx_t index = 0; index <= LEROBOT_QUANTILE_BIN_COUNT; index++) {
				// NumPy's linspace multiply and add are separately rounded.
				volatile T product = static_cast<T>(index) * step;
				auto edge = product;
				edge += low;
				edges[dimension][index] = edge;
			}
			edges[dimension].back() = high;
		}
	}

	vector<vector<int64_t>> counts;
	vector<vector<T>> edges;
};

template <class T>
LerobotHistogram<T> BuildHistogram(const LerobotStatsReaderFactory &reader_factory, idx_t row_width,
                                   idx_t dimension_offset, idx_t width, idx_t count, const vector<double> &minimum,
                                   const vector<double> &maximum) {
	LerobotHistogram<T> result(width, minimum, maximum);
	auto reader = reader_factory();
	if (reader->Count() != count) {
		throw InternalException("LeRobot statistics reader count changed from %llu to %llu", count, reader->Count());
	}
	vector<double> row;
	row.reserve(row_width);
	for (idx_t index = 0; index < count; index++) {
		ReadStatsRow(*reader, row_width, row);
		for (idx_t dimension = 0; dimension < width; dimension++) {
			const auto value = static_cast<T>(row[dimension_offset + dimension]);
			if (!std::isfinite(value)) {
				throw InvalidInputException("LeRobot numeric features cannot contain NaN or infinity");
			}
			const auto &edges = result.edges[dimension];
			auto iterator = std::upper_bound(edges.begin(), edges.end(), value);
			idx_t bin;
			if (iterator == edges.begin()) {
				bin = 0;
			} else if (iterator == edges.end()) {
				bin = LEROBOT_QUANTILE_BIN_COUNT - 1;
			} else {
				bin = NumericCast<idx_t>(iterator - edges.begin() - 1);
			}
			result.counts[dimension][bin]++;
		}
	}
	return result;
}

template <class T>
double HistogramQuantile(const vector<int64_t> &histogram, const vector<T> &edges, idx_t count, double quantile,
                         bool use_float32, bool &is_float32_result) {
	is_float32_result = use_float32;
	const auto target_count = quantile * static_cast<double>(count);
	int64_t cumulative = 0;
	for (idx_t bin = 0; bin < histogram.size(); bin++) {
		const auto before = cumulative;
		cumulative += histogram[bin];
		if (static_cast<double>(cumulative) < target_count) {
			continue;
		}
		if (bin == 0 || histogram[bin] == 0) {
			return static_cast<double>(edges[bin]);
		}
		const auto fraction = (target_count - static_cast<double>(before)) / static_cast<double>(histogram[bin]);
		if (use_float32) {
			is_float32_result = false;
			const auto bin_width = edges[bin + 1] - edges[bin];
			volatile double interpolation = fraction * static_cast<double>(bin_width);
			auto result = static_cast<double>(edges[bin]);
			result += interpolation;
			return result;
		}
		volatile double interpolation = fraction * static_cast<double>(edges[bin + 1] - edges[bin]);
		auto result = static_cast<double>(edges[bin]);
		result += interpolation;
		return result;
	}
	return static_cast<double>(edges.back());
}

template <class T>
LerobotFeatureStats ComputeStatsTyped(const LerobotStatsReaderFactory &reader_factory, idx_t width, idx_t output_count,
                                      bool use_float32) {
	if (width == 0 || output_count == 0) {
		throw InternalException("Cannot compute empty LeRobot feature statistics");
	}
	auto count_reader = reader_factory();
	const auto count = count_reader->Count();
	if (count == 0 || count > static_cast<idx_t>(NumericLimits<int64_t>::Maximum())) {
		throw InternalException("LeRobot statistics value count is empty or too large");
	}
	count_reader.reset();
	LerobotFeatureStats result;
	result.count = NumericCast<int64_t>(output_count);
	result.q01_is_float32 = use_float32;
	result.q10_is_float32 = use_float32;
	result.q50_is_float32 = use_float32;
	result.q90_is_float32 = use_float32;
	result.q99_is_float32 = use_float32;
	result.stddev_is_float32 = use_float32;
	result.min.reserve(width);
	result.max.reserve(width);
	result.mean.reserve(width);
	result.stddev.reserve(width);
	result.q01.reserve(width);
	result.q10.reserve(width);
	result.q50.reserve(width);
	result.q90.reserve(width);
	result.q99.reserve(width);

	for (idx_t dimension_offset = 0; dimension_offset < width;) {
		const auto batch_width = MinValue(LEROBOT_STATS_DIMENSION_BATCH_SIZE, width - dimension_offset);
		auto reader = reader_factory();
		if (reader->Count() != count) {
			throw InternalException("LeRobot statistics reader count changed from %llu to %llu", count,
			                        reader->Count());
		}
		vector<double> minimum(batch_width, std::numeric_limits<double>::infinity());
		vector<double> maximum(batch_width, -std::numeric_limits<double>::infinity());
		vector<double> row;
		row.reserve(width);
		if (count == 1) {
			ReadStatsRow(*reader, width, row);
			for (idx_t dimension = 0; dimension < batch_width; dimension++) {
				const auto value =
				    PrepareReductionValue<T>(row[dimension_offset + dimension], dimension, minimum, maximum);
				const auto output = static_cast<double>(value);
				result.min.push_back(output);
				result.max.push_back(output);
				result.mean.push_back(output);
				result.stddev.push_back(0);
				result.q01.push_back(output);
				result.q10.push_back(output);
				result.q50.push_back(output);
				result.q90.push_back(output);
				result.q99.push_back(output);
			}
			dimension_offset += batch_width;
			continue;
		}

		auto totals =
		    NumpyPairwiseStats<T>(*reader, width, dimension_offset, batch_width, count, minimum, maximum, row);
		for (idx_t dimension = 0; dimension < batch_width; dimension++) {
			const auto mean = totals.sum[dimension] / static_cast<T>(count);
			// RunningQuantileStats evaluates these expressions separately in the
			// input dtype. Volatile prevents contraction from changing the last ulp.
			volatile T average_square = totals.square_sum[dimension] / static_cast<T>(count);
			volatile T squared_mean = mean * mean;
			const auto variance = MaxValue(static_cast<T>(0), static_cast<T>(average_square - squared_mean));
			result.min.push_back(minimum[dimension]);
			result.max.push_back(maximum[dimension]);
			result.mean.push_back(static_cast<double>(mean));
			result.stddev.push_back(static_cast<double>(std::sqrt(variance)));
		}

		reader.reset();
		auto histogram =
		    BuildHistogram<T>(reader_factory, width, dimension_offset, batch_width, count, minimum, maximum);
		for (idx_t dimension = 0; dimension < batch_width; dimension++) {
			bool quantile_is_float32;
			result.q01.push_back(HistogramQuantile(histogram.counts[dimension], histogram.edges[dimension], count, 0.01,
			                                       use_float32, quantile_is_float32));
			result.q01_is_float32 = result.q01_is_float32 && quantile_is_float32;
			result.q10.push_back(HistogramQuantile(histogram.counts[dimension], histogram.edges[dimension], count, 0.10,
			                                       use_float32, quantile_is_float32));
			result.q10_is_float32 = result.q10_is_float32 && quantile_is_float32;
			result.q50.push_back(HistogramQuantile(histogram.counts[dimension], histogram.edges[dimension], count, 0.50,
			                                       use_float32, quantile_is_float32));
			result.q50_is_float32 = result.q50_is_float32 && quantile_is_float32;
			result.q90.push_back(HistogramQuantile(histogram.counts[dimension], histogram.edges[dimension], count, 0.90,
			                                       use_float32, quantile_is_float32));
			result.q90_is_float32 = result.q90_is_float32 && quantile_is_float32;
			result.q99.push_back(HistogramQuantile(histogram.counts[dimension], histogram.edges[dimension], count, 0.99,
			                                       use_float32, quantile_is_float32));
			result.q99_is_float32 = result.q99_is_float32 && quantile_is_float32;
		}
		dimension_offset += batch_width;
	}
	return result;
}

LerobotFeatureStats ComputeStats(const LerobotStatsReaderFactory &reader_factory, idx_t width, idx_t output_count,
                                 const LerobotFeature &feature) {
	if (UsesFloat32Statistics(feature)) {
		return ComputeStatsTyped<float>(reader_factory, width, output_count, true);
	}
	return ComputeStatsTyped<double>(reader_factory, width, output_count, false);
}

void MergeStats(LerobotFeatureStats &target, const LerobotFeatureStats &source) {
	if (target.count == 0) {
		target = source;
		return;
	}
	if (target.mean.size() != source.mean.size()) {
		throw InternalException("LeRobot dataset statistics width changed");
	}
	const auto target_count = static_cast<double>(target.count);
	const auto source_count = static_cast<double>(source.count);
	const auto total_count = target_count + source_count;
	for (idx_t index = 0; index < target.mean.size(); index++) {
		volatile double target_weighted_mean = target.mean[index] * target_count;
		volatile double source_weighted_mean = source.mean[index] * source_count;
		volatile double weighted_mean_sum = target_weighted_mean + source_weighted_mean;
		const auto total_mean = static_cast<double>(weighted_mean_sum / total_count);
		const auto target_delta = target.mean[index] - total_mean;
		const auto source_delta = source.mean[index] - total_mean;
		// aggregate_feature_stats squares every episode std array before
		// np.stack promotes mixed dtypes. Preserve that first float32 rounding,
		// then keep NumPy's subsequent float64 operations separate from FMA.
		double target_variance;
		if (target.stddev_is_float32) {
			volatile float squared =
			    static_cast<float>(target.stddev[index]) * static_cast<float>(target.stddev[index]);
			target_variance = squared;
		} else {
			volatile double squared = target.stddev[index] * target.stddev[index];
			target_variance = squared;
		}
		double source_variance;
		if (source.stddev_is_float32) {
			volatile float squared =
			    static_cast<float>(source.stddev[index]) * static_cast<float>(source.stddev[index]);
			source_variance = squared;
		} else {
			volatile double squared = source.stddev[index] * source.stddev[index];
			source_variance = squared;
		}
		volatile double target_delta_squared = target_delta * target_delta;
		volatile double source_delta_squared = source_delta * source_delta;
		volatile double target_component = target_variance + target_delta_squared;
		volatile double source_component = source_variance + source_delta_squared;
		volatile double target_weighted = target_component * target_count;
		volatile double source_weighted = source_component * source_count;
		volatile double weighted_variance_sum = target_weighted + source_weighted;
		const auto total_variance = static_cast<double>(weighted_variance_sum / total_count);
		target.min[index] = MinValue(target.min[index], source.min[index]);
		target.max[index] = MaxValue(target.max[index], source.max[index]);
		target.mean[index] = total_mean;
		target.stddev[index] = std::sqrt(MaxValue(0.0, total_variance));
		target.q01[index] = MinValue(target.q01[index], source.q01[index]);
		target.q10[index] = MinValue(target.q10[index], source.q10[index]);
		target.q50[index] = MinValue(target.q50[index], source.q50[index]);
		target.q90[index] = MaxValue(target.q90[index], source.q90[index]);
		target.q99[index] = MaxValue(target.q99[index], source.q99[index]);
	}
	target.count += source.count;
	// Multiplication by the int64 count promotes NumPy's aggregate mean and
	// variance to float64 after the first cross-episode merge.
	target.stddev_is_float32 = false;
}

void FlattenNumericValue(const Value &value, vector<double> &result) {
	if (value.IsNull()) {
		throw InvalidInputException("LeRobot feature values must not be NULL");
	}
	if (value.type().id() == LogicalTypeId::ARRAY) {
		for (const auto &child : ArrayValue::GetChildren(value)) {
			FlattenNumericValue(child, result);
		}
		return;
	}
	if (value.type().id() == LogicalTypeId::LIST) {
		for (const auto &child : ListValue::GetChildren(value)) {
			FlattenNumericValue(child, result);
		}
		return;
	}
	result.push_back(value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>());
}

vector<idx_t> SampleVisualFrameIndices(idx_t frame_count) {
	if (frame_count == 0) {
		return {};
	}
	const auto minimum_samples = MinValue<idx_t>(100, frame_count);
	const auto estimated = static_cast<idx_t>(std::pow(static_cast<double>(frame_count), 0.75));
	const auto sample_count = MaxValue(minimum_samples, MinValue<idx_t>(estimated, 10000));
	vector<idx_t> result;
	result.reserve(sample_count);
	if (sample_count == 1) {
		result.push_back(0);
		return result;
	}
	// np.linspace computes its floating step once, multiplies each arange
	// element by that rounded step, and finally overwrites the endpoint. Doing
	// `index * span / divisor` changes which side of a half-integer a few long
	// episode samples land on before np.round's ties-to-even operation.
	const auto step = static_cast<double>(frame_count - 1) / static_cast<double>(sample_count - 1);
	for (idx_t index = 0; index < sample_count; index++) {
		volatile double position = static_cast<double>(index) * step;
		if (index + 1 == sample_count) {
			position = static_cast<double>(frame_count - 1);
		}
		result.push_back(static_cast<idx_t>(std::nearbyint(position)));
	}
	return result;
}

LerobotRawVisualType InferRawVisualType(const LerobotFeature &feature, idx_t frame_size) {
	const auto width = feature.shape[1];
	const auto height = feature.shape[0];
	if (!feature.is_depth) {
		const auto expected = LerobotVisualWriter::ExpectedFrameBytes(width, height, LerobotRawVisualType::RGB24);
		if (frame_size != expected) {
			throw InvalidInputException(
			    "LeRobot RGB feature '%s' frame has %llu bytes; expected HWC uint8 RGB24 (%llu)", feature.name,
			    frame_size, expected);
		}
		return LerobotRawVisualType::RGB24;
	}
	const auto uint16_size = LerobotVisualWriter::ExpectedFrameBytes(width, height, LerobotRawVisualType::DEPTH_UINT16);
	const auto float32_size =
	    LerobotVisualWriter::ExpectedFrameBytes(width, height, LerobotRawVisualType::DEPTH_FLOAT32);
	if (frame_size == uint16_size) {
		return LerobotRawVisualType::DEPTH_UINT16;
	}
	if (frame_size == float32_size) {
		return LerobotRawVisualType::DEPTH_FLOAT32;
	}
	throw InvalidInputException(
	    "LeRobot depth feature '%s' frame has %llu bytes; expected HWC uint16 (%llu) or float32 (%llu)", feature.name,
	    frame_size, uint16_size, float32_size);
}

double ReadRawVisualValue(const string &frame, LerobotRawVisualType raw_type, idx_t element_index) {
	if (raw_type == LerobotRawVisualType::RGB24) {
		return static_cast<double>(static_cast<uint8_t>(frame[element_index]));
	}
	if (raw_type == LerobotRawVisualType::DEPTH_UINT16) {
		uint16_t value;
		memcpy(&value, frame.data() + element_index * sizeof(value), sizeof(value));
		return static_cast<double>(value);
	}
	float value;
	memcpy(&value, frame.data() + element_index * sizeof(value), sizeof(value));
	return static_cast<double>(value);
}

struct LerobotVisualSpool {
	void Initialize(string path_p) {
		path = std::move(path_p);
		frame_size = 0;
		frame_count = 0;
		write_handle.reset();
	}

	void Append(FileSystem &fs, const char *frame_data, idx_t size) {
		if (!write_handle) {
			fs.CreateDirectoriesRecursive(StringUtil::GetFilePath(path));
			write_handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
			                                     FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE);
			frame_size = size;
		} else if (size != frame_size) {
			throw InvalidInputException("LeRobot visual frames change size within an episode");
		}
		if (frame_count == NumericLimits<idx_t>::Maximum() ||
		    frame_size > NumericLimits<idx_t>::Maximum() / (frame_count + 1)) {
			throw InvalidInputException("LeRobot visual spool is too large");
		}
		auto written = write_handle->Write(const_cast<char *>(frame_data), size);
		if (written < 0 || static_cast<idx_t>(written) != size) {
			throw IOException("Failed to write a complete LeRobot visual frame to '%s'", path);
		}
		frame_count++;
	}

	void Close() {
		if (!write_handle) {
			return;
		}
		write_handle->Close();
		write_handle.reset();
	}

	void Remove(FileSystem &fs) {
		Close();
		if (!path.empty() && fs.FileExists(path)) {
			fs.RemoveFile(path);
		}
	}

	string path;
	unique_ptr<FileHandle> write_handle;
	idx_t frame_size = 0;
	idx_t frame_count = 0;
};

struct LerobotCollectionStatsReader : public LerobotStatsRowReader {
	LerobotCollectionStatsReader(const ColumnDataCollection &collection_p, const LerobotFeature &feature_p,
	                             idx_t width_p, idx_t fps_p)
	    : collection(collection_p), feature(feature_p), width(width_p), fps(fps_p),
	      generated_timestamp(!feature.user_defined && feature.name == "timestamp") {
		if (!generated_timestamp) {
			collection.InitializeScan(scan_state, {feature.output_index}, ColumnDataScanProperties::DISALLOW_ZERO_COPY);
			collection.InitializeScanChunk(scan_state, chunk);
		}
	}

	idx_t Count() const override {
		return collection.Count();
	}

	void Next(vector<double> &row) override {
		if (position >= Count()) {
			throw InternalException("LeRobot statistics reader advanced past the collection");
		}
		if (generated_timestamp) {
			row.push_back(static_cast<double>(position) / static_cast<double>(fps));
			position++;
			return;
		}
		while (chunk_position >= chunk.size()) {
			if (!collection.Scan(scan_state, chunk)) {
				throw InternalException("LeRobot statistics collection ended early");
			}
			chunk_position = 0;
		}
		FlattenNumericValue(chunk.GetValue(0, chunk_position++), row);
		if (row.size() != width) {
			throw InvalidInputException("LeRobot feature '%s' has %llu values; expected %llu", feature.name, row.size(),
			                            width);
		}
		position++;
	}

	const ColumnDataCollection &collection;
	const LerobotFeature &feature;
	idx_t width;
	idx_t fps;
	bool generated_timestamp;
	ColumnDataScanState scan_state;
	DataChunk chunk;
	idx_t chunk_position = 0;
	idx_t position = 0;
};

struct LerobotVisualStatsReader : public LerobotStatsRowReader {
	LerobotVisualStatsReader(FileSystem &fs, const LerobotFeature &feature_p, const LerobotVisualSpool &spool_p,
	                         LerobotRawVisualType raw_type_p, const vector<idx_t> &sampled_indices_p, idx_t step_p)
	    : feature(feature_p), spool(spool_p), raw_type(raw_type_p), sampled_indices(sampled_indices_p), step(step_p),
	      sampled_height(1 + (feature.shape[0] - 1) / step), sampled_width(1 + (feature.shape[1] - 1) / step) {
		const auto expected = LerobotVisualWriter::ExpectedFrameBytes(feature.shape[1], feature.shape[0], raw_type);
		if (spool.frame_size != expected || spool.frame_count == 0) {
			throw InvalidInputException("LeRobot visual feature '%s' changes raw frame type or dimensions",
			                            feature.name);
		}
		if (sampled_indices.empty()) {
			throw InternalException("LeRobot visual statistics sample cannot be empty");
		}
		if (sampled_height > NumericLimits<idx_t>::Maximum() / sampled_width ||
		    sampled_height * sampled_width > NumericLimits<idx_t>::Maximum() / sampled_indices.size()) {
			throw InvalidInputException("LeRobot visual statistics sample is too large");
		}
		values_per_frame = sampled_height * sampled_width;
		count = values_per_frame * sampled_indices.size();
		read_handle = fs.OpenFile(spool.path, FileFlags::FILE_FLAGS_READ);
		if (spool.frame_count > NumericLimits<idx_t>::Maximum() / spool.frame_size ||
		    read_handle->GetFileSize() != spool.frame_count * spool.frame_size) {
			throw IOException("LeRobot visual spool '%s' has an invalid size", spool.path);
		}
	}

	idx_t Count() const override {
		return count;
	}

	void Next(vector<double> &row) override {
		if (position >= count) {
			throw InternalException("LeRobot visual statistics reader advanced past its sample");
		}
		const auto sample_index = position / values_per_frame;
		if (sample_index != loaded_sample) {
			if (frame.empty()) {
				frame.resize(spool.frame_size);
			}
			const auto frame_index = sampled_indices[sample_index];
			if (frame_index >= spool.frame_count) {
				throw InternalException("LeRobot visual statistics sampled an invalid frame");
			}
			read_handle->Read(&frame[0], frame.size(), frame_index * frame.size());
			loaded_sample = sample_index;
		}
		const auto pixel_index = position % values_per_frame;
		const auto y = (pixel_index / sampled_width) * step;
		const auto x = (pixel_index % sampled_width) * step;
		const auto first_element = (y * feature.shape[1] + x) * feature.shape[2];
		for (idx_t channel = 0; channel < feature.shape[2]; channel++) {
			row.push_back(ReadRawVisualValue(frame, raw_type, first_element + channel));
		}
		position++;
	}

	const LerobotFeature &feature;
	const LerobotVisualSpool &spool;
	LerobotRawVisualType raw_type;
	const vector<idx_t> &sampled_indices;
	idx_t step;
	idx_t sampled_height;
	idx_t sampled_width;
	idx_t values_per_frame = 0;
	idx_t count = 0;
	unique_ptr<FileHandle> read_handle;
	string frame;
	idx_t loaded_sample = DConstants::INVALID_INDEX;
	idx_t position = 0;
};

void NormalizeRGBStats(LerobotFeatureStats &stats) {
	static const double RGB_MAX = 255.0;
	vector<vector<double> *> float32_values = {&stats.min, &stats.max, &stats.mean, &stats.stddev};
	for (auto entries : float32_values) {
		for (auto &value : *entries) {
			value = static_cast<double>(static_cast<float>(value) / static_cast<float>(RGB_MAX));
		}
	}
	// An edge-case quantile is a numpy.float32 while an interpolated quantile
	// is a numpy.float64. Building the per-channel array promotes it if any
	// channel interpolates, which also controls the division precision.
	vector<std::pair<vector<double> *, bool>> quantile_values = {{&stats.q01, stats.q01_is_float32},
	                                                             {&stats.q10, stats.q10_is_float32},
	                                                             {&stats.q50, stats.q50_is_float32},
	                                                             {&stats.q90, stats.q90_is_float32},
	                                                             {&stats.q99, stats.q99_is_float32}};
	for (auto &entry : quantile_values) {
		auto entries = entry.first;
		for (auto &value : *entries) {
			if (entry.second) {
				value = static_cast<double>(static_cast<float>(value) / static_cast<float>(RGB_MAX));
			} else {
				value /= RGB_MAX;
			}
		}
	}
}

LogicalType StatListType(const vector<idx_t> &shape, idx_t depth) {
	if (depth + 1 == shape.size()) {
		return LogicalType::LIST(LogicalType::DOUBLE);
	}
	return LogicalType::LIST(StatListType(shape, depth + 1));
}

Value BuildNestedStatValue(const vector<double> &values, const vector<idx_t> &shape, idx_t depth, idx_t &offset) {
	vector<Value> children;
	children.reserve(shape[depth]);
	if (depth + 1 == shape.size()) {
		for (idx_t index = 0; index < shape[depth]; index++) {
			if (offset >= values.size()) {
				throw InternalException("LeRobot statistic shape exceeds its value count");
			}
			children.push_back(Value::DOUBLE(values[offset++]));
		}
		return Value::LIST(LogicalType::DOUBLE, std::move(children));
	}
	for (idx_t index = 0; index < shape[depth]; index++) {
		children.push_back(BuildNestedStatValue(values, shape, depth + 1, offset));
	}
	return Value::LIST(StatListType(shape, depth + 1), std::move(children));
}

Value BuildStatValue(const vector<double> &values, const vector<idx_t> &shape) {
	idx_t offset = 0;
	auto result = BuildNestedStatValue(values, shape, 0, offset);
	if (offset != values.size()) {
		throw InternalException("LeRobot statistic shape did not consume all values");
	}
	return result;
}

string NestedStatsJSON(const vector<double> &values, const vector<idx_t> &shape, idx_t depth, idx_t &offset) {
	string result = "[";
	for (idx_t index = 0; index < shape[depth]; index++) {
		if (index > 0) {
			result += ',';
		}
		if (depth + 1 == shape.size()) {
			if (offset >= values.size()) {
				throw InternalException("LeRobot statistic JSON shape exceeds its value count");
			}
			result += JsonNumber(values[offset++]);
		} else {
			result += NestedStatsJSON(values, shape, depth + 1, offset);
		}
	}
	return result + ']';
}

string StatsJSONValue(const vector<double> &values, const vector<idx_t> &shape) {
	idx_t offset = 0;
	auto result = NestedStatsJSON(values, shape, 0, offset);
	if (offset != values.size()) {
		throw InternalException("LeRobot statistic JSON shape did not consume all values");
	}
	return result;
}

void WriteStringFile(FileSystem &fs, const string &path, const string &content) {
	auto parent = StringUtil::GetFilePath(path);
	if (!parent.empty()) {
		fs.CreateDirectoriesRecursive(parent);
	}
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
	                                    FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE);
	if (!content.empty()) {
		handle->Write(const_cast<char *>(content.data()), content.size());
	}
	handle->Close();
}

struct LerobotVideoRoute {
	idx_t chunk_index = 0;
	idx_t file_index = 0;
	double from_timestamp = 0;
	double to_timestamp = 0;
};

struct LerobotVideoShardState {
	bool initialized = false;
	idx_t chunk_index = 0;
	idx_t file_index = 0;
	double duration = 0;
};

struct LerobotCopyGlobalData : public GlobalFunctionData {
	LerobotCopyGlobalData(ClientContext &context_p, const LerobotCopyBindData &bind_p, string root_p)
	    : context(context_p), bind(bind_p), root(std::move(root_p)), fs(FileSystem::GetFileSystem(context)),
	      data_writer(bind.parquet_function,
	                  BindParquet(context, bind.parquet_function, bind.data_names, bind.data_types, "huggingface",
	                              HuggingFaceMetadataJSON(bind.features))),
	      episodes_writer(bind.parquet_function,
	                      BindParquet(context, bind.parquet_function, bind.episode_names, bind.episode_types)),
	      tasks_writer(bind.parquet_function,
	                   BindParquet(context, bind.parquet_function, {"task_index", "task"},
	                               {LogicalType::BIGINT, LogicalType::VARCHAR}, "pandas", TasksPandasMetadataJSON())) {
		StringUtil::RTrim(root, fs.PathSeparator(root));
		if (root.empty()) {
			throw IOException("LeRobot dataset root cannot be empty");
		}
		if (fs.IsRemoteFile(root)) {
			throw NotImplementedException("FORMAT lerobot currently requires a local dataset root");
		}
		if (fs.FileExists(root) || fs.DirectoryExists(root)) {
			throw IOException("LeRobot dataset root already exists: '%s'", root);
		}
		stage_root = root + ".tmp-" + UUID::ToString(UUID::GenerateRandomUUID());
		if (fs.FileExists(stage_root) || fs.DirectoryExists(stage_root)) {
			throw IOException("LeRobot staging root already exists: '%s'", stage_root);
		}
		fs.CreateDirectoriesRecursive(stage_root);
		metadata_buffer = make_uniq<ColumnDataCollection>(context, bind.episode_types,
		                                                  ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
		metadata_buffer->InitializeAppend(metadata_append_state);
		dataset_stats.resize(bind.features.size());
		dataset_stats_present.resize(bind.features.size(), false);
		visual_raw_types.resize(bind.features.size(), LerobotRawVisualType::RGB24);
		visual_raw_types_present.resize(bind.features.size(), false);
		video_shards.resize(bind.features.size());
		for (idx_t feature_index = 0; feature_index < bind.features.size(); feature_index++) {
			const auto &feature = bind.features[feature_index];
			if ((feature.is_image || feature.is_video) && !feature.is_depth) {
				visual_raw_types_present[feature_index] = true;
			}
		}
	}

	~LerobotCopyGlobalData() override {
		if (published || stage_root.empty()) {
			return;
		}
		try {
			for (auto &spool : current_visual_spools) {
				spool.Close();
			}
			data_writer.global_data.reset();
			episodes_writer.global_data.reset();
			tasks_writer.global_data.reset();
			if (fs.DirectoryExists(stage_root)) {
				fs.RemoveDirectory(stage_root);
			}
		} catch (...) { // NOLINT
		}
	}

	void StartEpisode(int64_t episode_index) {
		if (episode_index != NumericCast<int64_t>(total_episodes)) {
			throw InvalidInputException(
			    "LeRobot episodes must be contiguous and ordered from zero: expected %llu, got %lld", total_episodes,
			    episode_index);
		}
		current_episode = episode_index;
		current_episode_frames = 0;
		current_episode_tasks.clear();
		current_episode_task_set.clear();
		current_data = make_uniq<ColumnDataCollection>(context, bind.data_types,
		                                               ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
		current_data->InitializeAppend(current_data_append_state);
		current_visual_spools.clear();
		current_visual_spools.resize(bind.features.size());
		for (idx_t feature_index = 0; feature_index < bind.features.size(); feature_index++) {
			const auto &feature = bind.features[feature_index];
			if (!feature.is_image && !feature.is_video) {
				continue;
			}
			current_visual_spools[feature_index].Initialize(
			    fs.JoinPath(stage_root, ".tmp/raw/" + std::to_string(feature_index) + "/episode-" +
			                                std::to_string(current_episode) + ".raw"));
		}
		current_video_routes.clear();
		current_video_routes.resize(bind.features.size());
		has_current_episode = true;
	}

	LerobotRawVisualType ResolveRawVisualType(idx_t feature_index, idx_t frame_size) {
		const auto inferred = InferRawVisualType(bind.features[feature_index], frame_size);
		if (!visual_raw_types_present[feature_index]) {
			visual_raw_types[feature_index] = inferred;
			visual_raw_types_present[feature_index] = true;
		} else if (visual_raw_types[feature_index] != inferred) {
			throw InvalidInputException("LeRobot visual feature '%s' changes raw dtype within the dataset",
			                            bind.features[feature_index].name);
		}
		return inferred;
	}

	int64_t GetTaskIndex(const string &task) {
		auto entry = task_indexes.find(task);
		if (entry != task_indexes.end()) {
			return entry->second;
		}
		auto index = NumericCast<int64_t>(tasks.size());
		task_indexes.emplace(task, index);
		tasks.push_back(task);
		return index;
	}

	void AddEpisodeTask(const string &task) {
		if (current_episode_task_set.insert(task).second) {
			current_episode_tasks.push_back(task);
		}
	}

	void AppendRun(DataChunk &input, idx_t offset, idx_t count) {
		DataChunk output;
		output.Initialize(context, bind.data_types, count);
		vector<UnifiedVectorFormat> visual_formats(bind.features.size());
		for (idx_t feature_index = 0; feature_index < bind.features.size(); feature_index++) {
			const auto &feature = bind.features[feature_index];
			if (feature.is_image || feature.is_video) {
				input.data[feature.input_index].ToUnifiedFormat(input.size(), visual_formats[feature_index]);
				continue;
			}
			if (!feature.user_defined) {
				continue;
			}
			VectorOperations::Copy(input.data[feature.input_index], output.data[feature.output_index], offset + count,
			                       offset, 0);
		}

		for (idx_t row = 0; row < count; row++) {
			const auto input_row = offset + row;
			for (idx_t feature_index = 0; feature_index < bind.features.size(); feature_index++) {
				const auto &feature = bind.features[feature_index];
				if (!feature.is_image && !feature.is_video) {
					continue;
				}
				const auto &format = visual_formats[feature_index];
				const auto source_index = format.sel->get_index(input_row);
				if (!format.validity.RowIsValid(source_index)) {
					throw InvalidInputException("LeRobot feature '%s' must not contain NULL", feature.name);
				}
				const auto &raw_value = format.GetData<string_t>()[source_index];
				if (raw_value.GetSize() > bind.max_visual_frame_bytes) {
					throw InvalidInputException(
					    "LeRobot visual feature '%s' frame has %llu bytes, exceeding MAX_VISUAL_FRAME_BYTES=%llu",
					    feature.name, raw_value.GetSize(), bind.max_visual_frame_bytes);
				}
				const auto raw_type = ResolveRawVisualType(feature_index, raw_value.GetSize());
				current_visual_spools[feature_index].Append(fs, raw_value.GetData(), raw_value.GetSize());
				if (feature.is_image) {
					string raw_frame(raw_value.GetData(), raw_value.GetSize());
					auto encoded =
					    LerobotVisualWriter::EncodeImage(raw_frame, feature.shape[1], feature.shape[0], raw_type);
					output.data[feature.output_index].SetValue(
					    row, Value::STRUCT(
					             {{"bytes", Value::BLOB_RAW(encoded)},
					              {"path", Value(FrameFileName(current_episode_frames + row, feature.is_depth))}}));
				}
			}
			auto task_value = input.GetValue(bind.task_input_index, input_row);
			if (task_value.IsNull()) {
				throw InvalidInputException("LeRobot task values must not be NULL");
			}
			auto task = StringValue::Get(task_value);
			auto task_index = GetTaskIndex(task);
			AddEpisodeTask(task);
			const auto frame_index = current_episode_frames + row;
			const auto global_index = total_frames + frame_index;
			for (const auto &feature : bind.features) {
				if (feature.name == "timestamp") {
					output.data[feature.output_index].SetValue(
					    row, Value::FLOAT(static_cast<float>(frame_index) / static_cast<float>(bind.fps)));
				} else if (feature.name == "frame_index") {
					output.data[feature.output_index].SetValue(row, Value::BIGINT(NumericCast<int64_t>(frame_index)));
				} else if (feature.name == "episode_index") {
					output.data[feature.output_index].SetValue(row, Value::BIGINT(current_episode));
				} else if (feature.name == "index") {
					output.data[feature.output_index].SetValue(row, Value::BIGINT(NumericCast<int64_t>(global_index)));
				} else if (feature.name == "task_index") {
					output.data[feature.output_index].SetValue(row, Value::BIGINT(task_index));
				}
			}
		}
		output.SetCardinality(count);
		current_data->Append(current_data_append_state, output);
		current_episode_frames += count;
	}

	void Process(DataChunk &input) {
		for (const auto &feature : bind.features) {
			if (feature.user_defined && VectorOperations::HasNull(input.data[feature.input_index], input.size())) {
				throw InvalidInputException("LeRobot feature '%s' must not contain NULL", feature.name);
			}
		}
		idx_t offset = 0;
		while (offset < input.size()) {
			auto episode_value = input.GetValue(bind.episode_input_index, offset);
			if (episode_value.IsNull()) {
				throw InvalidInputException("LeRobot episode_index must not be NULL");
			}
			auto episode_index = episode_value.GetValue<int64_t>();
			if (!has_current_episode) {
				StartEpisode(episode_index);
			} else if (episode_index != current_episode) {
				FinishEpisode();
				StartEpisode(episode_index);
			}
			idx_t end = offset + 1;
			while (end < input.size()) {
				auto next = input.GetValue(bind.episode_input_index, end);
				if (next.IsNull() || next.GetValue<int64_t>() != episode_index) {
					break;
				}
				end++;
			}
			AppendRun(input, offset, end - offset);
			offset = end;
		}
	}

	void EnsureDataWriter(idx_t episode_length) {
		if (data_writer.global_data) {
			const auto frames_in_file = total_frames - data_file_start_frame;
			const auto size_limit = bind.data_file_size_mb * 1024.0 * 1024.0;
			const auto projection_factor =
			    frames_in_file > 0 ? 1.0 + static_cast<double>(episode_length) / frames_in_file : 1.0;
			if (data_writer.SizeAtLeast(size_limit / projection_factor)) {
				data_writer.Close(context);
				AdvanceFileIndex(data_chunk_index, data_file_index, bind.chunks_size);
				data_file_start_frame = total_frames;
			}
		}
		if (!data_writer.global_data) {
			auto path = fs.JoinPath(stage_root, DataRelativePath(data_chunk_index, data_file_index));
			fs.CreateDirectoriesRecursive(StringUtil::GetFilePath(path));
			data_writer.Open(context, path);
		}
	}

	idx_t GetLocalFileSize(const string &path) {
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
		return handle->GetFileSize();
	}

	string AbsoluteLocalPath(const string &path) {
		if (fs.IsPathAbsolute(path)) {
			return path;
		}
		return fs.JoinPath(FileSystem::GetWorkingDirectory(), path);
	}

	void EncodeEpisodeVideos() {
		for (idx_t feature_index = 0; feature_index < bind.features.size(); feature_index++) {
			const auto &feature = bind.features[feature_index];
			if (!feature.is_video) {
				continue;
			}
			if (!visual_raw_types_present[feature_index]) {
				throw InternalException("LeRobot video feature '%s' has no resolved raw dtype", feature.name);
			}
			auto temporary_dir = fs.JoinPath(stage_root, ".tmp/videos/" + std::to_string(feature_index));
			fs.CreateDirectoriesRecursive(temporary_dir);
			auto episode_path = fs.JoinPath(temporary_dir, "episode-" + std::to_string(current_episode) + ".mp4");
			LerobotVideoEncodeOptions options;
			options.width = feature.shape[1];
			options.height = feature.shape[0];
			options.fps = bind.fps;
			options.encoder_threads = bind.encoder_threads;
			options.raw_type = visual_raw_types[feature_index];
			const auto &spool = current_visual_spools[feature_index];
			if (spool.frame_count != current_episode_frames) {
				throw InternalException("LeRobot video spool has %llu frames for a %llu-frame episode",
				                        spool.frame_count, current_episode_frames);
			}
			auto encoded = LerobotVisualWriter::EncodeVideo(fs, episode_path, spool.path, spool.frame_count, options);
			if (encoded.frame_count != current_episode_frames) {
				throw InternalException("LeRobot video encoder wrote %llu frames for a %llu-frame episode",
				                        encoded.frame_count, current_episode_frames);
			}

			auto &shard = video_shards[feature_index];
			if (!shard.initialized) {
				shard.initialized = true;
				shard.chunk_index = 0;
				shard.file_index = 0;
				shard.duration = 0;
			} else {
				auto current_path =
				    fs.JoinPath(stage_root, VideoRelativePath(feature.name, shard.chunk_index, shard.file_index));
				const auto projected_size = static_cast<double>(GetLocalFileSize(current_path)) +
				                            static_cast<double>(GetLocalFileSize(episode_path));
				if (projected_size >= bind.video_file_size_mb * 1024.0 * 1024.0) {
					AdvanceFileIndex(shard.chunk_index, shard.file_index, bind.chunks_size);
					shard.duration = 0;
				}
			}

			auto shard_path =
			    fs.JoinPath(stage_root, VideoRelativePath(feature.name, shard.chunk_index, shard.file_index));
			fs.CreateDirectoriesRecursive(StringUtil::GetFilePath(shard_path));
			const auto from_timestamp = shard.duration;
			if (!fs.FileExists(shard_path)) {
				fs.MoveFile(episode_path, shard_path);
			} else {
				const auto absolute_shard = AbsoluteLocalPath(shard_path);
				const auto absolute_episode = AbsoluteLocalPath(episode_path);
				if (absolute_shard.find('\n') != string::npos || absolute_shard.find('\r') != string::npos ||
				    absolute_shard.find('\\') != string::npos || absolute_shard.find('\'') != string::npos ||
				    absolute_episode.find('\n') != string::npos || absolute_episode.find('\r') != string::npos ||
				    absolute_episode.find('\\') != string::npos || absolute_episode.find('\'') != string::npos) {
					throw InvalidInputException(
					    "LeRobot video output paths cannot contain newlines, backslashes, or single quotes");
				}
				auto concat_list =
				    fs.JoinPath(temporary_dir, "concat-" + std::to_string(current_episode) + ".ffconcat");
				auto concat_output =
				    fs.JoinPath(temporary_dir, "concatenated-" + std::to_string(current_episode) + ".mp4");
				WriteStringFile(fs, concat_list,
				                "ffconcat version 1.0\nfile '" + absolute_shard + "'\nfile '" + absolute_episode +
				                    "'\n");
				LerobotVisualWriter::ConcatenateVideos({absolute_shard, absolute_episode}, concat_list, concat_output);
				fs.RemoveFile(shard_path);
				fs.MoveFile(concat_output, shard_path);
				fs.RemoveFile(episode_path);
				fs.RemoveFile(concat_list);
			}
			shard.duration += encoded.duration;
			auto &route = current_video_routes[feature_index];
			route.chunk_index = shard.chunk_index;
			route.file_index = shard.file_index;
			route.from_timestamp = from_timestamp;
			route.to_timestamp = shard.duration;
		}
	}

	void CloseVisualSpools() {
		for (auto &spool : current_visual_spools) {
			spool.Close();
		}
	}

	void RemoveVisualSpools() {
		for (auto &spool : current_visual_spools) {
			spool.Remove(fs);
		}
	}

	LerobotFeatureStats ComputeEpisodeStats(idx_t feature_index) {
		const auto &feature = bind.features[feature_index];
		if (feature.is_image || feature.is_video) {
			const auto &spool = current_visual_spools[feature_index];
			if (spool.frame_count != current_episode_frames || !visual_raw_types_present[feature_index]) {
				throw InternalException("LeRobot visual feature '%s' has an incomplete episode spool", feature.name);
			}
			auto sampled_indices = SampleVisualFrameIndices(spool.frame_count);
			const auto largest_dimension = MaxValue(feature.shape[0], feature.shape[1]);
			idx_t step = 1;
			if (largest_dimension >= 300) {
				step = MaxValue<idx_t>(1, largest_dimension / 150);
			}
			LerobotStatsReaderFactory reader_factory = [&]() -> unique_ptr<LerobotStatsRowReader> {
				return make_uniq<LerobotVisualStatsReader>(fs, feature, spool, visual_raw_types[feature_index],
				                                           sampled_indices, step);
			};
			return ComputeStats(reader_factory, feature.shape[2], sampled_indices.size(), feature);
		}

		const auto width = ShapeWidth(feature.shape);
		LerobotStatsReaderFactory reader_factory = [&]() -> unique_ptr<LerobotStatsRowReader> {
			return make_uniq<LerobotCollectionStatsReader>(*current_data, feature, width, bind.fps);
		};
		return ComputeStats(reader_factory, width, current_episode_frames, feature);
	}

	void AddStatsToEpisodeRow(vector<Value> &row, const LerobotFeature &feature, const LerobotFeatureStats &stats) {
		vector<idx_t> shape = feature.shape;
		if (feature.is_image || feature.is_video) {
			shape = {feature.shape[2], 1, 1};
		}
		row.push_back(BuildStatValue(stats.min, shape));
		row.push_back(BuildStatValue(stats.max, shape));
		row.push_back(BuildStatValue(stats.mean, shape));
		row.push_back(BuildStatValue(stats.stddev, shape));
		row.push_back(Value::LIST(LogicalType::BIGINT, {Value::BIGINT(stats.count)}));
		row.push_back(BuildStatValue(stats.q01, shape));
		row.push_back(BuildStatValue(stats.q10, shape));
		row.push_back(BuildStatValue(stats.q50, shape));
		row.push_back(BuildStatValue(stats.q90, shape));
		row.push_back(BuildStatValue(stats.q99, shape));
	}

	void MaybeRotateEpisodesWriter(idx_t episode_length) {
		if (!episodes_writer.global_data) {
			return;
		}
		// Match LeRobotDatasetMetadata._save_episode_metadata: the latest
		// episode index is used as the divisor for its next-row size estimate.
		const auto latest_episode_index = current_episode > 0 ? static_cast<double>(current_episode - 1) : 0.0;
		const auto projection_factor =
		    latest_episode_index > 0 ? 1.0 + static_cast<double>(episode_length) / latest_episode_index : 1.0;
		const auto size_limit = bind.data_file_size_mb * 1024.0 * 1024.0;
		if (!episodes_writer.SizeAtLeast(size_limit / projection_factor)) {
			return;
		}
		FlushMetadataBuffer();
		episodes_writer.Close(context);
		AdvanceFileIndex(metadata_chunk_index, metadata_file_index, bind.chunks_size);
	}

	void AppendEpisodeMetadata(const vector<LerobotFeatureStats> &episode_stats, idx_t data_chunk, idx_t data_file,
	                           idx_t from_index, idx_t to_index) {
		MaybeRotateEpisodesWriter(current_episode_frames);
		vector<Value> row;
		row.push_back(Value::BIGINT(current_episode));
		vector<Value> episode_task_values;
		for (const auto &task : current_episode_tasks) {
			episode_task_values.emplace_back(task);
		}
		row.push_back(Value::LIST(LogicalType::VARCHAR, std::move(episode_task_values)));
		row.push_back(Value::BIGINT(NumericCast<int64_t>(current_episode_frames)));
		row.push_back(Value::BIGINT(NumericCast<int64_t>(data_chunk)));
		row.push_back(Value::BIGINT(NumericCast<int64_t>(data_file)));
		row.push_back(Value::BIGINT(NumericCast<int64_t>(from_index)));
		row.push_back(Value::BIGINT(NumericCast<int64_t>(to_index)));
		for (idx_t feature_index = 0; feature_index < bind.features.size(); feature_index++) {
			if (!bind.features[feature_index].is_video) {
				continue;
			}
			const auto &route = current_video_routes[feature_index];
			row.push_back(Value::BIGINT(NumericCast<int64_t>(route.chunk_index)));
			row.push_back(Value::BIGINT(NumericCast<int64_t>(route.file_index)));
			row.push_back(Value::DOUBLE(route.from_timestamp));
			row.push_back(Value::DOUBLE(route.to_timestamp));
		}
		for (idx_t feature_index = 0; feature_index < bind.features.size(); feature_index++) {
			if (bind.features[feature_index].HasStatistics()) {
				AddStatsToEpisodeRow(row, bind.features[feature_index], episode_stats[feature_index]);
			}
		}
		row.push_back(Value::BIGINT(NumericCast<int64_t>(metadata_chunk_index)));
		row.push_back(Value::BIGINT(NumericCast<int64_t>(metadata_file_index)));
		if (row.size() != bind.episode_types.size()) {
			throw InternalException("LeRobot episode metadata row has %llu values, expected %llu", row.size(),
			                        bind.episode_types.size());
		}
		DataChunk chunk;
		chunk.Initialize(context, bind.episode_types, 1);
		for (idx_t column = 0; column < row.size(); column++) {
			chunk.data[column].SetValue(0, row[column]);
		}
		chunk.SetCardinality(1);
		metadata_buffer->Append(metadata_append_state, chunk);
		metadata_buffer_rows++;
		if (metadata_buffer_rows >= bind.metadata_buffer_size) {
			FlushMetadataBuffer();
		}
	}

	void FinishEpisode() {
		if (!has_current_episode) {
			return;
		}
		if (current_episode_frames == 0) {
			throw InvalidInputException("LeRobot episodes cannot be empty");
		}
		CloseVisualSpools();
		vector<LerobotFeatureStats> episode_stats(bind.features.size());
		for (idx_t feature_index = 0; feature_index < bind.features.size(); feature_index++) {
			if (!bind.features[feature_index].HasStatistics()) {
				continue;
			}
			const auto &feature = bind.features[feature_index];
			episode_stats[feature_index] = ComputeEpisodeStats(feature_index);
			if ((feature.is_image || feature.is_video) && !feature.is_depth) {
				NormalizeRGBStats(episode_stats[feature_index]);
			}
		}

		EnsureDataWriter(current_episode_frames);
		const auto episode_data_chunk = data_chunk_index;
		const auto episode_data_file = data_file_index;
		data_writer.Flush(context, std::move(current_data));
		EncodeEpisodeVideos();
		RemoveVisualSpools();

		for (idx_t feature_index = 0; feature_index < bind.features.size(); feature_index++) {
			if (!bind.features[feature_index].HasStatistics()) {
				continue;
			}
			MergeStats(dataset_stats[feature_index], episode_stats[feature_index]);
			dataset_stats_present[feature_index] = true;
		}
		const auto from_index = total_frames;
		const auto to_index = total_frames + current_episode_frames;
		AppendEpisodeMetadata(episode_stats, episode_data_chunk, episode_data_file, from_index, to_index);
		total_frames = to_index;
		total_episodes++;
		has_current_episode = false;
	}

	void FlushMetadataBuffer() {
		if (!metadata_buffer || metadata_buffer_rows == 0) {
			return;
		}
		if (!episodes_writer.global_data) {
			auto path = fs.JoinPath(stage_root, EpisodesRelativePath(metadata_chunk_index, metadata_file_index));
			fs.CreateDirectoriesRecursive(StringUtil::GetFilePath(path));
			episodes_writer.Open(context, path);
		}
		episodes_writer.Flush(context, std::move(metadata_buffer));
		metadata_buffer = make_uniq<ColumnDataCollection>(context, bind.episode_types,
		                                                  ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
		metadata_buffer->InitializeAppend(metadata_append_state);
		metadata_buffer_rows = 0;
	}

	void WriteTasks() {
		if (tasks.empty()) {
			return;
		}
		vector<LogicalType> types = {LogicalType::BIGINT, LogicalType::VARCHAR};
		auto collection =
		    make_uniq<ColumnDataCollection>(context, types, ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR);
		ColumnDataAppendState append_state;
		collection->InitializeAppend(append_state);
		idx_t offset = 0;
		while (offset < tasks.size()) {
			const auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, tasks.size() - offset);
			DataChunk chunk;
			chunk.Initialize(context, types, count);
			for (idx_t row = 0; row < count; row++) {
				chunk.data[0].SetValue(row, Value::BIGINT(NumericCast<int64_t>(offset + row)));
				chunk.data[1].SetValue(row, Value(tasks[offset + row]));
			}
			chunk.SetCardinality(count);
			collection->Append(append_state, chunk);
			offset += count;
		}
		auto path = fs.JoinPath(stage_root, "meta/tasks.parquet");
		fs.CreateDirectoriesRecursive(StringUtil::GetFilePath(path));
		tasks_writer.Open(context, path);
		tasks_writer.Flush(context, std::move(collection));
		tasks_writer.Close(context);
	}

	string FeatureStatsJSON(idx_t feature_index) const {
		const auto &feature = bind.features[feature_index];
		const auto &stats = dataset_stats[feature_index];
		vector<idx_t> shape = feature.shape;
		if (feature.is_image || feature.is_video) {
			shape = {feature.shape[2], 1, 1};
		}
		return "{\"min\":" + StatsJSONValue(stats.min, shape) + ",\"max\":" + StatsJSONValue(stats.max, shape) +
		       ",\"mean\":" + StatsJSONValue(stats.mean, shape) + ",\"std\":" + StatsJSONValue(stats.stddev, shape) +
		       ",\"count\":[" + std::to_string(stats.count) + "],\"q01\":" + StatsJSONValue(stats.q01, shape) +
		       ",\"q10\":" + StatsJSONValue(stats.q10, shape) + ",\"q50\":" + StatsJSONValue(stats.q50, shape) +
		       ",\"q90\":" + StatsJSONValue(stats.q90, shape) + ",\"q99\":" + StatsJSONValue(stats.q99, shape) + '}';
	}

	string StatsJSON() const {
		string result = "{";
		bool first = true;
		for (idx_t index = 0; index < bind.features.size(); index++) {
			if (!dataset_stats_present[index]) {
				continue;
			}
			if (!first) {
				result += ',';
			}
			first = false;
			result += JsonEscape(bind.features[index].name) + ':' + FeatureStatsJSON(index);
		}
		return result + '}';
	}

	string VisualInfoJSON(idx_t feature_index) const {
		const auto &feature = bind.features[feature_index];
		if (!feature.is_image && !feature.is_video) {
			return string();
		}
		if (!visual_raw_types_present[feature_index]) {
			return feature.is_depth ? "{\"is_depth_map\":true}" : string();
		}
		const auto depth = feature.is_depth;
		const auto depth_unit = visual_raw_types[feature_index] == LerobotRawVisualType::DEPTH_UINT16 ? "mm" : "m";
		if (feature.is_image) {
			return depth ? "{\"is_depth_map\":true,\"depth_unit\":" + JsonEscape(depth_unit) + '}' : string();
		}
		string result =
		    "{\"video.height\":" + std::to_string(feature.shape[0]) +
		    ",\"video.width\":" + std::to_string(feature.shape[1]) +
		    ",\"video.codec\":" + JsonEscape(depth ? "hevc" : "av1") +
		    ",\"video.pix_fmt\":" + JsonEscape(depth ? "gray12le" : "yuv420p") +
		    ",\"video.fps\":" + std::to_string(bind.fps) + ",\"video.channels\":" + std::to_string(feature.shape[2]) +
		    ",\"has_audio\":false,\"video.g\":2,\"video.crf\":30,\"video.preset\":" + (depth ? "null" : "12") +
		    ",\"video.fast_decode\":0,\"video.video_backend\":\"pyav\",\"video.extra_options\":" +
		    (depth ? "{\"x265-params\":\"lossless=1\"}" : "{}") + ",\"is_depth_map\":" + (depth ? "true" : "false");
		if (depth) {
			result += ",\"video.depth_min\":0.01,\"video.depth_max\":10,\"video.shift\":3.5,"
			          "\"video.use_log\":true,\"depth_unit\":" +
			          JsonEscape(depth_unit);
		}
		return result + '}';
	}

	string FeaturesJSON() const {
		string result = "{";
		for (idx_t index = 0; index < bind.features.size(); index++) {
			const auto &feature = bind.features[index];
			if (index > 0) {
				result += ',';
			}
			result += JsonEscape(feature.name) + ":{\"dtype\":" + JsonEscape(feature.dtype) +
			          ",\"shape\":" + ShapeJSON(feature.shape) + ",\"names\":" + feature.names_json;
			auto visual_info = VisualInfoJSON(index);
			if (!visual_info.empty()) {
				result += ",\"info\":" + visual_info;
			}
			result += '}';
		}
		return result + '}';
	}

	string InfoJSON() const {
		bool has_video = false;
		for (const auto &feature : bind.features) {
			has_video = has_video || feature.is_video;
		}
		return "{\"codebase_version\":\"" + string(LEROBOT_CODEBASE_VERSION) +
		       "\",\"fps\":" + std::to_string(bind.fps) + ",\"features\":" + FeaturesJSON() +
		       ",\"total_episodes\":" + std::to_string(total_episodes) +
		       ",\"total_frames\":" + std::to_string(total_frames) +
		       ",\"total_tasks\":" + std::to_string(tasks.size()) +
		       ",\"chunks_size\":" + std::to_string(bind.chunks_size) +
		       ",\"data_files_size_in_mb\":" + JsonNumber(bind.data_file_size_mb) +
		       ",\"video_files_size_in_mb\":" + JsonNumber(bind.video_file_size_mb) +
		       ",\"data_path\":" + JsonEscape(LEROBOT_DATA_PATH) +
		       ",\"video_path\":" + (has_video ? JsonEscape(LEROBOT_VIDEO_PATH) : "null") +
		       ",\"robot_type\":" + (bind.has_robot_type ? JsonEscape(bind.robot_type) : "null") + ",\"splits\":{" +
		       (total_episodes == 0 ? string() : "\"train\":\"0:" + std::to_string(total_episodes) + "\"") + "}}";
	}

	void Finalize() {
		FinishEpisode();
		FlushMetadataBuffer();
		data_writer.Close(context);
		episodes_writer.Close(context);
		WriteTasks();
		auto temporary_root = fs.JoinPath(stage_root, ".tmp");
		if (fs.DirectoryExists(temporary_root)) {
			fs.RemoveDirectory(temporary_root);
		}
		if (total_episodes > 0) {
			WriteStringFile(fs, fs.JoinPath(stage_root, "meta/stats.json"), StatsJSON());
		}
		WriteStringFile(fs, fs.JoinPath(stage_root, "meta/info.json"), InfoJSON());
		fs.MoveFile(stage_root, root);
		published = true;
	}

	ClientContext &context;
	const LerobotCopyBindData &bind;
	string root;
	string stage_root;
	FileSystem &fs;
	DelegatedParquetWriter data_writer;
	DelegatedParquetWriter episodes_writer;
	DelegatedParquetWriter tasks_writer;
	unique_ptr<ColumnDataCollection> current_data;
	ColumnDataAppendState current_data_append_state;
	unique_ptr<ColumnDataCollection> metadata_buffer;
	ColumnDataAppendState metadata_append_state;
	idx_t metadata_buffer_rows = 0;
	unordered_map<string, int64_t> task_indexes;
	vector<string> tasks;
	vector<string> current_episode_tasks;
	unordered_set<string> current_episode_task_set;
	vector<LerobotFeatureStats> dataset_stats;
	vector<bool> dataset_stats_present;
	vector<LerobotVisualSpool> current_visual_spools;
	vector<LerobotRawVisualType> visual_raw_types;
	vector<bool> visual_raw_types_present;
	vector<LerobotVideoRoute> current_video_routes;
	vector<LerobotVideoShardState> video_shards;
	idx_t total_episodes = 0;
	idx_t total_frames = 0;
	int64_t current_episode = -1;
	idx_t current_episode_frames = 0;
	idx_t data_chunk_index = 0;
	idx_t data_file_index = 0;
	idx_t data_file_start_frame = 0;
	idx_t metadata_chunk_index = 0;
	idx_t metadata_file_index = 0;
	bool has_current_episode = false;
	bool published = false;
};

unique_ptr<LocalFunctionData> LerobotCopyInitializeLocal(ExecutionContext &, FunctionData &) {
	return make_uniq<LerobotCopyLocalData>();
}

unique_ptr<GlobalFunctionData> LerobotCopyInitializeGlobal(ClientContext &context, FunctionData &bind_data,
                                                           const string &path) {
	return make_uniq<LerobotCopyGlobalData>(context, bind_data.Cast<LerobotCopyBindData>(), path);
}

void LerobotCopySink(ExecutionContext &, FunctionData &, GlobalFunctionData &global_data, LocalFunctionData &,
                     DataChunk &input) {
	global_data.Cast<LerobotCopyGlobalData>().Process(input);
}

void LerobotCopyCombine(ExecutionContext &, FunctionData &, GlobalFunctionData &, LocalFunctionData &) {
}

void LerobotCopyFinalize(ClientContext &, FunctionData &, GlobalFunctionData &global_data) {
	global_data.Cast<LerobotCopyGlobalData>().Finalize();
}

CopyFunctionExecutionMode LerobotCopyExecutionMode(bool, bool) {
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}

} // namespace

CopyFunction LerobotCopyFunction::Create() {
	CopyFunction function("lerobot");
	function.copy_options = LerobotCopyOptions;
	function.copy_to_bind = LerobotCopyBind;
	function.copy_to_initialize_local = LerobotCopyInitializeLocal;
	function.copy_to_initialize_global = LerobotCopyInitializeGlobal;
	function.copy_to_sink = LerobotCopySink;
	function.copy_to_combine = LerobotCopyCombine;
	function.copy_to_finalize = LerobotCopyFinalize;
	function.execution_mode = LerobotCopyExecutionMode;
	function.extension = "lerobot";
	return function;
}

} // namespace duckdb
