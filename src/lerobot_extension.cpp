#include "lerobot_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

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

void LoadInternal(ExtensionLoader &loader) {
	// This is the stable layout contract used by the scan and decoder work that
	// follows. It deliberately performs no I/O, so it works for local paths and
	// URI roots (hf://, s3://, https://) alike.
	TableFunction layout("lerobot_layout", {LogicalType::VARCHAR}, LerobotLayoutFunction, LerobotLayoutBind);
	loader.RegisterFunction(layout);
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
