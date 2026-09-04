#include "function/lerobot_functions.hpp"

#include "compat/lerobot_bind_replace.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

#include "lerobot_path.hpp"
#include "storage/lerobot_metadata_cache.hpp"

#include <algorithm>

namespace duckdb {

namespace {

static const char *LEROBOT_INFO_SUFFIX = "/meta/info.json";
static const char *LEROBOT_EPISODES_SUFFIX = "/meta/episodes/**/*.parquet";
static const char *LEROBOT_TASKS_SUFFIX = "/meta/tasks.parquet";

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

string GetRoot(TableFunctionBindInput &input, const char *function_name) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("%s root must not be NULL", function_name);
	}
	return NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
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

named_parameter_map_t GetParquetParameters(TableFunctionBindInput &input) {
	auto parameters = input.named_parameters;
	parameters.erase("episode_indices");
	parameters.erase("refresh");
	return parameters;
}

void MaybeInvalidateCaches(ClientContext &context, const string &root, TableFunctionBindInput &input) {
	if (GetRefreshParameter(input)) {
		LerobotDatasetMetadata::Invalidate(context, root);
	}
}

unique_ptr<TableRef> LerobotInfoBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto root = GetRoot(input, "lerobot_info");
	MaybeInvalidateCaches(context, root, input);
	return LerobotCreateTableFunctionRef("read_json_auto", Value(root + LEROBOT_INFO_SUFFIX));
}

unique_ptr<TableRef> LerobotEpisodesBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto root = GetRoot(input, "lerobot_episodes");
	MaybeInvalidateCaches(context, root, input);
	auto info = ReadLerobotDatasetInfo(context, root);
	auto parameters = GetParquetParameters(input);
	if (info.total_episodes == 0) {
		return LerobotCreateEmptyParquetRelation(context, std::move(info.episode_schema.names),
		                                         std::move(info.episode_schema.types), parameters);
	}
	return LerobotCreateTableFunctionRef("read_parquet", Value(root + LEROBOT_EPISODES_SUFFIX), parameters);
}

unique_ptr<TableRef> LerobotTasksBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto root = GetRoot(input, "lerobot_tasks");
	MaybeInvalidateCaches(context, root, input);
	auto info = ReadLerobotDatasetInfo(context, root);
	if (info.total_tasks == 0) {
		return LerobotCreateEmptyParquetRelation(context, {"task_index", "task"},
		                                         {LogicalType::BIGINT, LogicalType::VARCHAR});
	}
	return LerobotCreateTableFunctionRef("read_parquet", Value(root + LEROBOT_TASKS_SUFFIX));
}

unique_ptr<TableRef> CreateFilteredFrameScan(vector<string> paths, const named_parameter_map_t &parameters,
                                             const vector<int64_t> &episode_indices) {
	auto select = make_uniq<SelectNode>();
	select->select_list.push_back(make_uniq<StarExpression>());
	select->from_table = LerobotCreateTableFunctionRef("read_parquet", LerobotCreatePathList(paths), parameters);

	vector<unique_ptr<ParsedExpression>> filter_children;
	filter_children.push_back(make_uniq<ColumnRefExpression>("episode_index"));
	for (const auto episode_index : episode_indices) {
		filter_children.push_back(make_uniq<ConstantExpression>(Value::BIGINT(episode_index)));
	}
	if (episode_indices.empty()) {
		select->where_clause = make_uniq<ConstantExpression>(Value::BOOLEAN(false));
	} else {
		select->where_clause = make_uniq<OperatorExpression>(ExpressionType::COMPARE_IN, std::move(filter_children));
	}

	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return make_uniq<SubqueryRef>(std::move(statement));
}

unique_ptr<TableRef> LerobotScanBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto root = GetRoot(input, "lerobot_scan");
	auto requested_episodes = input.named_parameters.find("episode_indices");
	vector<int64_t> episode_indices;
	if (requested_episodes != input.named_parameters.end()) {
		episode_indices = GetEpisodeIndices(requested_episodes->second, "lerobot_scan");
	}
	const auto refresh = GetRefreshParameter(input);
	auto metadata = LerobotDatasetMetadata::Get(context, root, refresh);
	auto parameters = GetParquetParameters(input);

	if (requested_episodes == input.named_parameters.end()) {
		if (metadata->GetDataFiles().empty()) {
			const auto &schema = metadata->GetFrameSchema();
			return LerobotCreateEmptyParquetRelation(context, schema.names, schema.types, parameters);
		}
		return LerobotCreateTableFunctionRef("read_parquet", LerobotCreatePathList(metadata->GetDataFiles()),
		                                     parameters);
	}

	auto data_files = metadata->ResolveDataFiles(episode_indices);
	if (data_files.empty()) {
		if (!metadata->GetDataFiles().empty()) {
			// An empty selection on a non-empty dataset still has the native
			// Parquet schema. Bind one authoritative shard but filter every row.
			data_files.push_back(metadata->GetDataFiles().front());
			return CreateFilteredFrameScan(std::move(data_files), parameters, episode_indices);
		}
		const auto &schema = metadata->GetFrameSchema();
		return LerobotCreateEmptyParquetRelation(context, schema.names, schema.types, parameters);
	}
	return CreateFilteredFrameScan(std::move(data_files), parameters, episode_indices);
}

struct LerobotCacheInfoBindData final : public TableFunctionData {
	LerobotCacheInfoBindData(string root_p, shared_ptr<LerobotDatasetMetadata> data_p,
	                         shared_ptr<LerobotVideoMetadata> video_p)
	    : root(std::move(root_p)), data(std::move(data_p)), video(std::move(video_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotCacheInfoBindData>(root, data, video);
	}

	string root;
	shared_ptr<LerobotDatasetMetadata> data;
	shared_ptr<LerobotVideoMetadata> video;
};

struct LerobotCacheInfoGlobalState final : public GlobalTableFunctionState {
	idx_t next_component = 0;
};

unique_ptr<GlobalTableFunctionState> LerobotCacheInfoInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<LerobotCacheInfoGlobalState>();
}

unique_ptr<FunctionData> LerobotCacheInfoBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	auto root = GetRoot(input, "lerobot_cache_info");
	names = {"root", "component", "cached", "entries", "bytes"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BIGINT,
	                LogicalType::UBIGINT};
	return make_uniq<LerobotCacheInfoBindData>(root, LerobotDatasetMetadata::Peek(context, root),
	                                           LerobotVideoMetadata::Peek(context, root));
}

void LerobotCacheInfoFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotCacheInfoBindData>();
	auto &state = input.global_state->Cast<LerobotCacheInfoGlobalState>();
	idx_t count = 0;
	while (state.next_component < 2 && count < STANDARD_VECTOR_SIZE) {
		const auto data_component = state.next_component == 0;
		const auto cached = data_component ? bind_data.data != nullptr : bind_data.video != nullptr;
		idx_t bytes = 0;
		if (cached) {
			auto estimate =
			    data_component ? bind_data.data->GetEstimatedCacheMemory() : bind_data.video->GetEstimatedCacheMemory();
			if (estimate.IsValid()) {
				bytes = estimate.GetIndex();
			}
		}
		output.data[0].SetValue(count, Value(bind_data.root));
		output.data[1].SetValue(count, Value(data_component ? "data" : "video"));
		output.data[2].SetValue(count, Value::BOOLEAN(cached));
		output.data[3].SetValue(count, Value::BIGINT(cached ? 1 : 0));
		output.data[4].SetValue(count, Value::UBIGINT(bytes));
		state.next_component++;
		count++;
	}
	output.SetCardinality(count);
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
	auto root = GetRoot(input, "lerobot_video_routes");
	auto episode_indices = GetEpisodeIndices(input.inputs[1], "lerobot_video_routes");

	auto metadata = LerobotVideoMetadata::Get(context, root, GetRefreshParameter(input));
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
	output.SetCardinality(count);
}

void AddParquetScanParameters(TableFunction &function) {
	function.named_parameters["union_by_name"] = LogicalType::BOOLEAN;
	function.named_parameters["binary_as_string"] = LogicalType::BOOLEAN;
	function.named_parameters["filename"] = LogicalType::ANY;
	function.named_parameters["file_row_number"] = LogicalType::BOOLEAN;
}

TableFunction CreateBindReplaceFunction(const char *name, table_function_bind_replace_t bind_replace) {
	TableFunction function(name, {LogicalType::VARCHAR}, nullptr, nullptr);
	function.bind_replace = bind_replace;
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	return function;
}

} // namespace

TableFunctionSet LerobotFunctions::GetInfoFunction() {
	return TableFunctionSet(CreateBindReplaceFunction("lerobot_info", LerobotInfoBindReplace));
}

TableFunctionSet LerobotFunctions::GetEpisodesFunction() {
	auto function = CreateBindReplaceFunction("lerobot_episodes", LerobotEpisodesBindReplace);
	AddParquetScanParameters(function);
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetTasksFunction() {
	return TableFunctionSet(CreateBindReplaceFunction("lerobot_tasks", LerobotTasksBindReplace));
}

TableFunctionSet LerobotFunctions::GetScanFunction() {
	auto function = CreateBindReplaceFunction("lerobot_scan", LerobotScanBindReplace);
	function.named_parameters["episode_indices"] = LogicalType::LIST(LogicalType::BIGINT);
	AddParquetScanParameters(function);
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetCacheInfoFunction() {
	TableFunction function("lerobot_cache_info", {LogicalType::VARCHAR}, LerobotCacheInfoFunction, LerobotCacheInfoBind,
	                       LerobotCacheInfoInit);
	return TableFunctionSet(std::move(function));
}

TableFunctionSet LerobotFunctions::GetVideoRoutesFunction() {
	TableFunction function("lerobot_video_routes", {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::BIGINT)},
	                       LerobotVideoRoutesFunction, LerobotVideoRoutesBind, LerobotVideoRoutesInit);
	function.named_parameters["video_keys"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	return TableFunctionSet(std::move(function));
}

} // namespace duckdb
