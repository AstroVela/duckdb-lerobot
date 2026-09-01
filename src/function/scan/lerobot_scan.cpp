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
#include <type_traits>

namespace duckdb {

namespace {

template <typename CALLBACK>
struct BindColumnNames;

template <typename RESULT, typename CONTEXT, typename INPUT, typename RETURN_TYPES, typename COLUMN_NAMES>
struct BindColumnNames<RESULT (*)(CONTEXT, INPUT, RETURN_TYPES, COLUMN_NAMES)> {
	using type = typename std::remove_reference<COLUMN_NAMES>::type;
};

// DuckDB 2.0 uses vector<Identifier> here, while the Vane fork currently uses
// vector<string>. Derive the type from DuckDB's callback ABI so both builds use
// the same source without version checks.
using LerobotColumnNames = typename BindColumnNames<table_function_bind_t>::type;

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
	case LerobotScanKind::FRAMES:
		return "/data/**/*.parquet";
	default:
		throw InternalException("Unknown LeRobot scan kind");
	}
}

unique_ptr<MultiFileReader> CreateLerobotReader(LerobotScanKind kind, const TableFunction &) {
	return make_uniq<LerobotMultiFileReader>(kind);
}

template <typename FUNCTION_SET, typename CALLBACK>
auto ModifyFunctions(FUNCTION_SET &functions, CALLBACK &callback, int)
    -> decltype(functions.ApplyToFunctions(callback), void()) {
	functions.ApplyToFunctions(callback);
}

template <typename FUNCTION_SET, typename CALLBACK>
void ModifyFunctions(FUNCTION_SET &functions, CALLBACK &callback, long) {
	for (auto &function : functions.functions) {
		callback(function);
	}
}

template <typename FUNCTION>
auto SetFunctionName(FUNCTION &function, const char *name, int) -> decltype(function.SetName(name), void()) {
	function.SetName(name);
}

template <typename FUNCTION>
void SetFunctionName(FUNCTION &function, const char *name, long) {
	function.name = name;
}

template <typename CHUNK>
auto SetOutputCardinality(CHUNK &output, idx_t count, int) -> decltype(output.SetCardinalityUnsafe(count), void()) {
	output.SetCardinalityUnsafe(count);
}

template <typename CHUNK>
void SetOutputCardinality(CHUNK &output, idx_t count, long) {
	output.SetCardinality(count);
}

TableFunctionSet CreateNativeScan(ExtensionLoader &loader, const char *source_name, const char *target_name,
                                  table_function_get_multi_file_reader_t create_reader) {
	// Follow the Iceberg extension's scan construction: copy the native reader
	// from the catalog, then inject only LeRobot's dataset-root expansion.
	auto &source = loader.GetTableFunction(source_name);
	auto result = source.functions;
	auto modify = [target_name, create_reader](TableFunction &function) {
		function.get_multi_file_reader = create_reader;
		SetFunctionName(function, target_name, 0);
	};
	ModifyFunctions(result, modify, 0);
	SetFunctionName(result, target_name, 0);
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

unique_ptr<FunctionData> LerobotLayoutBind(ClientContext &, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, LerobotColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_layout root must not be NULL");
	}

	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	names = {"root", "info_path", "episodes_path", "data_path", "videos_path"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR};
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
	output.data[3].SetValue(0, Value(root + "/data"));
	output.data[4].SetValue(0, Value(root + "/videos"));
	SetOutputCardinality(output, 1, 0);
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
                                                 vector<LogicalType> &return_types, LerobotColumnNames &names) {
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
	SetOutputCardinality(output, 1, 0);
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
                                                  vector<LogicalType> &return_types, LerobotColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_metadata_cache root must not be NULL");
	}
	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	bool cache_hit;
	auto metadata = LerobotDatasetMetadata::Get(context, root, GetRefreshParameter(input), cache_hit);

	names = {"root", "codebase_version", "data_path", "episode_count", "data_file_count", "cache_hit"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
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
	output.data[3].SetValue(0, Value::BIGINT(static_cast<int64_t>(metadata.GetEpisodeCount())));
	output.data[4].SetValue(0, Value::BIGINT(static_cast<int64_t>(metadata.GetDataFileCount())));
	output.data[5].SetValue(0, Value::BOOLEAN(bind_data.cache_hit));
	SetOutputCardinality(output, 1, 0);
	state.emitted = true;
}

unique_ptr<TableRef> LerobotEpisodeFramesBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_episode_frames root must not be NULL");
	}
	if (input.inputs[1].IsNull()) {
		throw BinderException("lerobot_episode_frames episode_indices must not be NULL");
	}

	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	vector<int64_t> episode_indices;
	for (const auto &value : ListValue::GetChildren(input.inputs[1])) {
		if (value.IsNull()) {
			throw BinderException("lerobot_episode_frames episode_indices must not contain NULL");
		}
		auto episode_index = value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		if (episode_index < 0) {
			throw BinderException("LeRobot episode indices must be non-negative");
		}
		episode_indices.push_back(episode_index);
	}
	std::sort(episode_indices.begin(), episode_indices.end());
	episode_indices.erase(std::unique(episode_indices.begin(), episode_indices.end()), episode_indices.end());

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

TableFunctionSet LerobotFunctions::GetFramesFunction(ExtensionLoader &loader) {
	return CreateNativeScan(loader, "parquet_scan", "lerobot_frames", LerobotMultiFileReader::CreateFrames);
}

TableFunctionSet LerobotFunctions::GetMetadataCacheFunction() {
	TableFunction function("lerobot_metadata_cache", {LogicalType::VARCHAR}, LerobotMetadataCacheFunction,
	                       LerobotMetadataCacheBind, LerobotSingleRowInit);
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
