#include "function/lerobot_functions.hpp"

#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

#include "function/lerobot_multi_file_reader.hpp"
#include "storage/lerobot_metadata_cache.hpp"

#include <algorithm>

namespace duckdb {

namespace {

bool IsHuggingFaceRepoId(const string &root) {
	if (root.empty() || root[0] == '/' || StringUtil::Contains(root, "://") || StringUtil::StartsWith(root, "./") ||
	    StringUtil::StartsWith(root, "../")) {
		return false;
	}

	const auto slash = root.find('/');
	if (slash == string::npos || slash == 0 || slash + 1 == root.size() || root.find('/', slash + 1) != string::npos) {
		return false;
	}
	for (const auto ch : root) {
		if (StringUtil::CharacterIsAlphaNumeric(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/') {
			continue;
		}
		return false;
	}
	return true;
}

string ScanSuffix(LerobotScanKind kind) {
	switch (kind) {
	case LerobotScanKind::INFO:
		return "/meta/info.json";
	case LerobotScanKind::EPISODES:
		return "/meta/episodes/**/*.parquet";
	case LerobotScanKind::TASKS:
		return "/meta/tasks.parquet";
	case LerobotScanKind::FRAMES:
		return "/data/**/*.parquet";
	default:
		throw InternalException("Unknown LeRobot scan kind");
	}
}

unique_ptr<MultiFileReader> CreateLerobotReader(LerobotScanKind kind, const TableFunction &) {
	return make_uniq<LerobotMultiFileReader>(kind);
}

void SetOutputCardinality(DataChunk &output, idx_t count) {
	output.SetCardinality(count);
}

TableFunctionSet CreateNativeScan(ExtensionLoader &loader, const char *source_name, const char *target_name,
                                  table_function_get_multi_file_reader_t create_reader) {
	// Follow the Iceberg extension's scan construction: copy the native reader
	// from the catalog, then inject only LeRobot's dataset-root expansion.
	auto &source = loader.GetTableFunction(source_name);
	auto result = source.functions;
	for (auto &function : result.functions) {
		function.get_multi_file_reader = create_reader;
		function.name = target_name;
	}
	result.name = target_name;
	return result;
}

struct LerobotLayoutBindData final : public TableFunctionData {
	explicit LerobotLayoutBindData(string root_p) : root(std::move(root_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotLayoutBindData>(root);
	}

	string root;
};

struct LerobotSingleRowGlobalState final : public GlobalTableFunctionState {
	bool emitted = false;
};

unique_ptr<GlobalTableFunctionState> LerobotSingleRowInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<LerobotSingleRowGlobalState>();
}

bool GetRefreshParameter(TableFunctionBindInput &input) {
	auto refresh = input.named_parameters.find("refresh");
	if (refresh == input.named_parameters.end()) {
		return false;
	}
	if (refresh->second.IsNull()) {
		throw BinderException("refresh must not be NULL");
	}
	return BooleanValue::Get(refresh->second);
}

vector<int64_t> GetEpisodeIndices(const Value &value, const char *function_name) {
	if (value.IsNull()) {
		throw BinderException("%s episode_indices must not be NULL", function_name);
	}

	vector<int64_t> episode_indices;
	for (const auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw BinderException("%s episode_indices must not contain NULL", function_name);
		}
		auto episode_index = child.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		if (episode_index < 0) {
			throw BinderException("LeRobot episode indices must be non-negative");
		}
		episode_indices.push_back(episode_index);
	}
	std::sort(episode_indices.begin(), episode_indices.end());
	episode_indices.erase(std::unique(episode_indices.begin(), episode_indices.end()), episode_indices.end());
	return episode_indices;
}

unique_ptr<FunctionData> LerobotLayoutBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_layout root must not be NULL");
	}

	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	names = {"root", "info_path", "episodes_path", "tasks_path", "data_path", "videos_path"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return make_uniq<LerobotLayoutBindData>(std::move(root));
}

void LerobotLayoutFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotLayoutBindData>();
	auto &state = input.global_state->Cast<LerobotSingleRowGlobalState>();
	if (state.emitted) {
		return;
	}

	const auto &root = bind_data.root;
	output.data[0].SetValue(0, Value(root));
	output.data[1].SetValue(0, Value(root + "/meta/info.json"));
	output.data[2].SetValue(0, Value(root + "/meta/episodes"));
	output.data[3].SetValue(0, Value(root + "/meta/tasks.parquet"));
	output.data[4].SetValue(0, Value(root + "/data"));
	output.data[5].SetValue(0, Value(root + "/videos"));
	SetOutputCardinality(output, 1);
	state.emitted = true;
}

string FormatShardIndex(int64_t index) {
	if (index < 0) {
		throw BinderException("LeRobot shard indices must be non-negative");
	}
	auto formatted = std::to_string(index);
	if (formatted.size() >= 3) {
		return formatted;
	}
	return string(3 - formatted.size(), '0') + formatted;
}

struct LerobotV3ShardBindData final : public TableFunctionData {
	LerobotV3ShardBindData(string root_p, string video_key_p, int64_t chunk_index_p, int64_t file_index_p)
	    : root(std::move(root_p)), video_key(std::move(video_key_p)), chunk_index(chunk_index_p),
	      file_index(file_index_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotV3ShardBindData>(root, video_key, chunk_index, file_index);
	}

	string root;
	string video_key;
	int64_t chunk_index;
	int64_t file_index;
};

unique_ptr<FunctionData> LerobotV3ShardPathsBind(ClientContext &, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	for (const auto &value : input.inputs) {
		if (value.IsNull()) {
			throw BinderException("lerobot_v3_shard_paths arguments must not be NULL");
		}
	}

	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	auto video_key = StringValue::Get(input.inputs[1]);
	if (video_key.empty()) {
		throw BinderException("lerobot_v3_shard_paths video_key must not be empty");
	}
	auto chunk_index = BigIntValue::Get(input.inputs[2]);
	auto file_index = BigIntValue::Get(input.inputs[3]);
	FormatShardIndex(chunk_index);
	FormatShardIndex(file_index);

	names = {"episodes_path", "data_path", "video_path"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return make_uniq<LerobotV3ShardBindData>(std::move(root), std::move(video_key), chunk_index, file_index);
}

void LerobotV3ShardPathsFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotV3ShardBindData>();
	auto &state = input.global_state->Cast<LerobotSingleRowGlobalState>();
	if (state.emitted) {
		return;
	}

	const auto chunk = FormatShardIndex(bind_data.chunk_index);
	const auto file = FormatShardIndex(bind_data.file_index);
	const auto &root = bind_data.root;
	output.data[0].SetValue(0, Value(root + "/meta/episodes/chunk-" + chunk + "/file-" + file + ".parquet"));
	output.data[1].SetValue(0, Value(root + "/data/chunk-" + chunk + "/file-" + file + ".parquet"));
	output.data[2].SetValue(
	    0, Value(root + "/videos/" + bind_data.video_key + "/chunk-" + chunk + "/file-" + file + ".mp4"));
	SetOutputCardinality(output, 1);
	state.emitted = true;
}

unique_ptr<TableFunctionRef> CreateTableFunctionRef(const char *name, Value argument) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(std::move(argument)));
	auto result = make_uniq<TableFunctionRef>();
	result->function = make_uniq<FunctionExpression>(name, std::move(children));
	return result;
}

Value CreatePathList(const vector<string> &paths) {
	vector<Value> values;
	values.reserve(paths.size());
	for (const auto &path : paths) {
		values.push_back(Value(path));
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(values));
}

struct LerobotMetadataCacheBindData final : public TableFunctionData {
	LerobotMetadataCacheBindData(shared_ptr<LerobotDatasetMetadata> metadata_p, bool cache_hit_p)
	    : metadata(std::move(metadata_p)), cache_hit(cache_hit_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotMetadataCacheBindData>(metadata, cache_hit);
	}

	shared_ptr<LerobotDatasetMetadata> metadata;
	bool cache_hit;
};

unique_ptr<FunctionData> LerobotMetadataCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_metadata_cache root must not be NULL");
	}
	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	bool cache_hit;
	auto metadata = LerobotDatasetMetadata::Get(context, root, GetRefreshParameter(input), cache_hit);

	names = {"root",          "codebase_version", "data_path",       "video_path", "fps",
	         "episode_count", "data_file_count",  "video_key_count", "cache_hit"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::BIGINT,  LogicalType::BIGINT,
	                LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::BOOLEAN};
	return make_uniq<LerobotMetadataCacheBindData>(std::move(metadata), cache_hit);
}

void LerobotMetadataCacheFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotMetadataCacheBindData>();
	auto &state = input.global_state->Cast<LerobotSingleRowGlobalState>();
	if (state.emitted) {
		return;
	}

	auto &metadata = *bind_data.metadata;
	output.data[0].SetValue(0, Value(metadata.GetRoot()));
	output.data[1].SetValue(0, Value(metadata.GetCodebaseVersion()));
	output.data[2].SetValue(0, Value(metadata.GetDataPathTemplate()));
	output.data[3].SetValue(0, Value(metadata.GetVideoPathTemplate()));
	output.data[4].SetValue(0, Value::BIGINT(metadata.GetFPS()));
	output.data[5].SetValue(0, Value::BIGINT(static_cast<int64_t>(metadata.GetEpisodeCount())));
	output.data[6].SetValue(0, Value::BIGINT(static_cast<int64_t>(metadata.GetDataFileCount())));
	output.data[7].SetValue(0, Value::BIGINT(static_cast<int64_t>(metadata.GetVideoKeyCount())));
	output.data[8].SetValue(0, Value::BOOLEAN(bind_data.cache_hit));
	SetOutputCardinality(output, 1);
	state.emitted = true;
}

struct LerobotVideoMetadataCacheBindData final : public TableFunctionData {
	LerobotVideoMetadataCacheBindData(shared_ptr<LerobotVideoMetadata> metadata_p, bool cache_hit_p)
	    : metadata(std::move(metadata_p)), cache_hit(cache_hit_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotVideoMetadataCacheBindData>(metadata, cache_hit);
	}

	shared_ptr<LerobotVideoMetadata> metadata;
	bool cache_hit;
};

unique_ptr<FunctionData> LerobotVideoMetadataCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_video_metadata_cache root must not be NULL");
	}
	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	bool cache_hit;
	auto metadata = LerobotVideoMetadata::Get(context, root, GetRefreshParameter(input), cache_hit);

	names = {"root", "video_path", "fps", "video_key_count", "route_count", "video_file_count", "cache_hit"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::BOOLEAN};
	return make_uniq<LerobotVideoMetadataCacheBindData>(std::move(metadata), cache_hit);
}

void LerobotVideoMetadataCacheFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoMetadataCacheBindData>();
	auto &state = input.global_state->Cast<LerobotSingleRowGlobalState>();
	if (state.emitted) {
		return;
	}

	auto &metadata = *bind_data.metadata;
	output.data[0].SetValue(0, Value(metadata.GetRoot()));
	output.data[1].SetValue(0, Value(metadata.GetVideoPathTemplate()));
	output.data[2].SetValue(0, Value::BIGINT(metadata.GetFPS()));
	output.data[3].SetValue(0, Value::BIGINT(static_cast<int64_t>(metadata.GetVideoKeys().size())));
	output.data[4].SetValue(0, Value::BIGINT(static_cast<int64_t>(metadata.GetRouteCount())));
	output.data[5].SetValue(0, Value::BIGINT(static_cast<int64_t>(metadata.GetVideoFileCount())));
	output.data[6].SetValue(0, Value::BOOLEAN(bind_data.cache_hit));
	SetOutputCardinality(output, 1);
	state.emitted = true;
}

struct LerobotVideoRoutesBindData final : public TableFunctionData {
	LerobotVideoRoutesBindData(shared_ptr<LerobotVideoMetadata> metadata_p, vector<LerobotVideoRoute> routes_p)
	    : metadata(std::move(metadata_p)), routes(std::move(routes_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotVideoRoutesBindData>(metadata, routes);
	}

	shared_ptr<LerobotVideoMetadata> metadata;
	vector<LerobotVideoRoute> routes;
};

struct LerobotVideoRoutesGlobalState final : public GlobalTableFunctionState {
	idx_t next_route = 0;
};

unique_ptr<GlobalTableFunctionState> LerobotVideoRoutesInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<LerobotVideoRoutesGlobalState>();
}

unique_ptr<FunctionData> LerobotVideoRoutesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_video_routes root must not be NULL");
	}
	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	auto episode_indices = GetEpisodeIndices(input.inputs[1], "lerobot_video_routes");

	bool cache_hit;
	auto metadata = LerobotVideoMetadata::Get(context, root, GetRefreshParameter(input), cache_hit);
	vector<string> video_keys;
	auto requested_keys = input.named_parameters.find("video_keys");
	if (requested_keys == input.named_parameters.end()) {
		video_keys = metadata->GetVideoKeys();
	} else {
		if (requested_keys->second.IsNull()) {
			throw BinderException("lerobot_video_routes video_keys must not be NULL");
		}
		for (const auto &value : ListValue::GetChildren(requested_keys->second)) {
			if (value.IsNull()) {
				throw BinderException("lerobot_video_routes video_keys must not contain NULL");
			}
			auto video_key = StringValue::Get(value);
			if (video_key.empty()) {
				throw BinderException("lerobot_video_routes video_keys must not contain an empty key");
			}
			video_keys.push_back(std::move(video_key));
		}
		std::sort(video_keys.begin(), video_keys.end());
		video_keys.erase(std::unique(video_keys.begin(), video_keys.end()), video_keys.end());
	}

	names = {"episode_index", "video_key",      "video_path",   "chunk_index",
	         "file_index",    "from_timestamp", "to_timestamp", "fps"};
	return_types = {LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::DOUBLE,  LogicalType::DOUBLE,  LogicalType::BIGINT};
	auto routes = metadata->ResolveRoutes(episode_indices, video_keys);
	return make_uniq<LerobotVideoRoutesBindData>(std::move(metadata), std::move(routes));
}

void LerobotVideoRoutesFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoRoutesBindData>();
	auto &state = input.global_state->Cast<LerobotVideoRoutesGlobalState>();
	idx_t count = 0;
	while (state.next_route < bind_data.routes.size() && count < STANDARD_VECTOR_SIZE) {
		const auto &route = bind_data.routes[state.next_route++];
		output.data[0].SetValue(count, Value::BIGINT(route.episode_index));
		output.data[1].SetValue(count, Value(bind_data.metadata->GetVideoKey(route)));
		output.data[2].SetValue(count, Value(bind_data.metadata->GetVideoFile(route)));
		output.data[3].SetValue(count, Value::BIGINT(route.chunk_index));
		output.data[4].SetValue(count, Value::BIGINT(route.file_index));
		output.data[5].SetValue(count, Value::DOUBLE(route.from_timestamp));
		output.data[6].SetValue(count, Value::DOUBLE(route.to_timestamp));
		output.data[7].SetValue(count, Value::BIGINT(bind_data.metadata->GetFPS()));
		count++;
	}
	SetOutputCardinality(output, count);
}

unique_ptr<TableRef> LerobotEpisodeFramesBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_episode_frames root must not be NULL");
	}

	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	auto episode_indices = GetEpisodeIndices(input.inputs[1], "lerobot_episode_frames");

	vector<unique_ptr<ParsedExpression>> filter_children;
	filter_children.push_back(make_uniq<ColumnRefExpression>("episode_index"));
	for (const auto episode_index : episode_indices) {
		filter_children.push_back(make_uniq<ConstantExpression>(Value::BIGINT(episode_index)));
	}

	bool cache_hit;
	auto metadata = LerobotDatasetMetadata::Get(context, root, GetRefreshParameter(input), cache_hit);
	auto data_files = metadata->ResolveDataFiles(episode_indices);
	if (data_files.empty() && !metadata->GetSchemaDataFile().empty()) {
		// DuckDB still needs one footer to bind the output schema for an empty or
		// unknown episode set. The false/IN predicate prevents data rows from it.
		data_files.push_back(metadata->GetSchemaDataFile());
	}

	auto select = make_uniq<SelectNode>();
	select->select_list.push_back(make_uniq<StarExpression>());
	if (data_files.empty()) {
		select->from_table = CreateTableFunctionRef("lerobot_frames", Value(std::move(root)));
	} else {
		select->from_table = CreateTableFunctionRef("parquet_scan", CreatePathList(data_files));
	}
	if (filter_children.size() == 1) {
		select->where_clause = make_uniq<ConstantExpression>(Value::BOOLEAN(false));
	} else {
		select->where_clause = make_uniq<OperatorExpression>(ExpressionType::COMPARE_IN, std::move(filter_children));
	}

	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return make_uniq<SubqueryRef>(std::move(statement));
}

} // namespace

string NormalizeLerobotRoot(string root) {
	StringUtil::Trim(root);
	while (root.size() > 1 && root.back() == '/') {
		root.pop_back();
	}
	if (root.empty()) {
		throw BinderException("LeRobot dataset root must not be empty");
	}
	if (IsHuggingFaceRepoId(root)) {
		return "hf://datasets/" + root;
	}
	return root;
}

LerobotMultiFileReader::LerobotMultiFileReader(LerobotScanKind kind_p) : kind(kind_p) {
}

unique_ptr<MultiFileReader> LerobotMultiFileReader::CreateInfo(const TableFunction &function) {
	return CreateLerobotReader(LerobotScanKind::INFO, function);
}

unique_ptr<MultiFileReader> LerobotMultiFileReader::CreateEpisodes(const TableFunction &function) {
	return CreateLerobotReader(LerobotScanKind::EPISODES, function);
}

unique_ptr<MultiFileReader> LerobotMultiFileReader::CreateTasks(const TableFunction &function) {
	return CreateLerobotReader(LerobotScanKind::TASKS, function);
}

unique_ptr<MultiFileReader> LerobotMultiFileReader::CreateFrames(const TableFunction &function) {
	return CreateLerobotReader(LerobotScanKind::FRAMES, function);
}

vector<string> LerobotMultiFileReader::ParsePaths(const Value &input) {
	auto roots = MultiFileReader::ParsePaths(input);
	if (roots.size() != 1) {
		throw BinderException("LeRobot scans require exactly one dataset root");
	}
	return {NormalizeLerobotRoot(std::move(roots[0])) + ScanSuffix(kind)};
}

unique_ptr<MultiFileReader> LerobotMultiFileReader::Copy() const {
	return make_uniq<LerobotMultiFileReader>(kind);
}

TableFunctionSet LerobotFunctions::GetLayoutFunction() {
	TableFunction function("lerobot_layout", {LogicalType::VARCHAR}, LerobotLayoutFunction, LerobotLayoutBind,
	                       LerobotSingleRowInit);
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetV3ShardPathsFunction() {
	TableFunction function("lerobot_v3_shard_paths",
	                       {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT},
	                       LerobotV3ShardPathsFunction, LerobotV3ShardPathsBind, LerobotSingleRowInit);
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetInfoFunction(ExtensionLoader &loader) {
	return CreateNativeScan(loader, "read_json_auto", "lerobot_info", LerobotMultiFileReader::CreateInfo);
}

TableFunctionSet LerobotFunctions::GetEpisodesFunction(ExtensionLoader &loader) {
	return CreateNativeScan(loader, "parquet_scan", "lerobot_episodes", LerobotMultiFileReader::CreateEpisodes);
}

TableFunctionSet LerobotFunctions::GetTasksFunction(ExtensionLoader &loader) {
	return CreateNativeScan(loader, "parquet_scan", "lerobot_tasks", LerobotMultiFileReader::CreateTasks);
}

TableFunctionSet LerobotFunctions::GetFramesFunction(ExtensionLoader &loader) {
	return CreateNativeScan(loader, "parquet_scan", "lerobot_frames", LerobotMultiFileReader::CreateFrames);
}

TableFunctionSet LerobotFunctions::GetMetadataCacheFunction() {
	TableFunction function("lerobot_metadata_cache", {LogicalType::VARCHAR}, LerobotMetadataCacheFunction,
	                       LerobotMetadataCacheBind, LerobotSingleRowInit);
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetVideoMetadataCacheFunction() {
	TableFunction function("lerobot_video_metadata_cache", {LogicalType::VARCHAR}, LerobotVideoMetadataCacheFunction,
	                       LerobotVideoMetadataCacheBind, LerobotSingleRowInit);
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetVideoRoutesFunction() {
	TableFunction function("lerobot_video_routes", {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::BIGINT)},
	                       LerobotVideoRoutesFunction, LerobotVideoRoutesBind, LerobotVideoRoutesInit);
	function.named_parameters["video_keys"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetEpisodeFramesFunction() {
	TableFunction function("lerobot_episode_frames", {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::BIGINT)},
	                       nullptr, nullptr);
	function.bind_replace = LerobotEpisodeFramesBindReplace;
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	return TableFunctionSet(std::move(function));
}

} // namespace duckdb
