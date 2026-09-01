#include "lerobot_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

namespace {

struct LerobotLayoutBindData final : public TableFunctionData {
	explicit LerobotLayoutBindData(string root_p) : root(std::move(root_p)) {
}

	string root;
	bool emitted = false;
};

string NormalizeRoot(string root) {
	while (root.size() > 1 && root.back() == '/') {
		root.pop_back();
	}
	return root;
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

unique_ptr<FunctionData> LerobotLayoutBind(ClientContext &, TableFunctionBindInput &input,
	                                         vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_layout root must not be NULL");
	}

	auto root = NormalizeRoot(StringValue::Get(input.inputs[0]));
	if (root.empty()) {
		throw BinderException("lerobot_layout root must not be empty");
	}

	names = {"root", "info_path", "episodes_path", "data_path", "videos_path"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR};
	return make_uniq<LerobotLayoutBindData>(std::move(root));
}

void LerobotLayoutFunction(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotLayoutBindData>();
	if (bind_data.emitted) {
		return;
	}

	const auto &root = bind_data.root;
	output.SetValue(0, 0, Value(root));
	output.SetValue(1, 0, Value(root + "/meta/info.json"));
	output.SetValue(2, 0, Value(root + "/meta/episodes"));
	output.SetValue(3, 0, Value(root + "/data"));
	output.SetValue(4, 0, Value(root + "/videos"));
	output.SetCardinality(1);
	bind_data.emitted = true;
}

struct LerobotV3ShardBindData final : public TableFunctionData {
	LerobotV3ShardBindData(string root_p, string video_key_p, int64_t chunk_index_p, int64_t file_index_p)
	    : root(std::move(root_p)), video_key(std::move(video_key_p)), chunk_index(chunk_index_p), file_index(file_index_p) {
	}

	string root;
	string video_key;
	int64_t chunk_index;
	int64_t file_index;
	bool emitted = false;
};

unique_ptr<FunctionData> LerobotV3ShardPathsBind(ClientContext &, TableFunctionBindInput &input,
	                                                vector<LogicalType> &return_types, vector<string> &names) {
	for (const auto &value : input.inputs) {
		if (value.IsNull()) {
			throw BinderException("lerobot_v3_shard_paths arguments must not be NULL");
		}
	}

	auto root = NormalizeRoot(StringValue::Get(input.inputs[0]));
	auto video_key = StringValue::Get(input.inputs[1]);
	if (root.empty() || video_key.empty()) {
		throw BinderException("lerobot_v3_shard_paths root and video_key must not be empty");
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
	if (bind_data.emitted) {
		return;
	}

	const auto chunk = FormatShardIndex(bind_data.chunk_index);
	const auto file = FormatShardIndex(bind_data.file_index);
	const auto &root = bind_data.root;
	output.SetValue(0, 0, Value(root + "/meta/episodes/chunk-" + chunk + "/file-" + file + ".parquet"));
	output.SetValue(1, 0, Value(root + "/data/chunk-" + chunk + "/file-" + file + ".parquet"));
	output.SetValue(2, 0,
	                Value(root + "/videos/" + bind_data.video_key + "/chunk-" + chunk + "/file-" + file + ".mp4"));
	output.SetCardinality(1);
	bind_data.emitted = true;
}

unique_ptr<CreateMacroInfo> CreateTableMacro(const string &name, const vector<string> &parameters, const string &query) {
	Parser parser;
	parser.ParseQuery(query);
	if (parser.statements.size() != 1 || parser.statements[0]->type != StatementType::SELECT_STATEMENT) {
		throw InternalException("Expected a single SELECT statement for %s", name);
	}

	auto node = std::move(parser.statements[0]->Cast<SelectStatement>().node);
	auto macro = make_uniq<TableMacroFunction>(std::move(node));
	for (const auto &parameter : parameters) {
		macro->parameters.push_back(make_uniq<ColumnRefExpression>(parameter));
	}

	auto info = make_uniq<CreateMacroInfo>(CatalogType::TABLE_MACRO_ENTRY);
	info->name = name;
	info->temporary = true;
	info->internal = true;
	info->macros.push_back(std::move(macro));
	return info;
}

unique_ptr<CreateMacroInfo> CreateLerobotEpisodesMacro() {
	return CreateTableMacro("lerobot_episodes", {"root"}, R"(
		SELECT *
		FROM read_parquet(
			root || '/meta/episodes/**/*.parquet',
			union_by_name = true
		)
	)");
}

unique_ptr<CreateMacroInfo> CreateLerobotInfoMacro() {
	return CreateTableMacro("lerobot_info", {"root"}, R"(
		SELECT *
		FROM read_json(root || '/meta/info.json')
	)");
}

unique_ptr<CreateMacroInfo> CreateLerobotEpisodeFramesMacro() {
	return CreateTableMacro("lerobot_episode_frames", {"root", "episode_indices"}, R"(
		SELECT *
		FROM read_parquet(
			root || '/data/**/*.parquet',
			union_by_name = true
		)
		WHERE list_contains(episode_indices, episode_index)
	)");
}

void LoadInternal(ExtensionLoader &loader) {
	// This is the stable layout contract used by the scan and decoder work that
	// follows. It deliberately performs no I/O, so it works for local paths and
	// URI roots (hf://, s3://, https://) alike.
	TableFunction layout("lerobot_layout", {LogicalType::VARCHAR}, LerobotLayoutFunction, LerobotLayoutBind);
	loader.RegisterFunction(layout);
	TableFunction shard_paths("lerobot_v3_shard_paths",
	                          {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT},
	                          LerobotV3ShardPathsFunction, LerobotV3ShardPathsBind);
	loader.RegisterFunction(shard_paths);
	auto episodes = CreateLerobotEpisodesMacro();
	loader.RegisterFunction(*episodes);
	auto info = CreateLerobotInfoMacro();
	loader.RegisterFunction(*info);
	auto frames = CreateLerobotEpisodeFramesMacro();
	loader.RegisterFunction(*frames);
}

} // namespace

void LerobotExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string LerobotExtension::Name() {
	return "lerobot";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(lerobot, loader) {
	duckdb::LoadInternal(loader);
}

}
