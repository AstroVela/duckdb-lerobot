#include "duckdb.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "core_functions_extension.hpp"
#include "json_extension.hpp"
#include "parquet_extension.hpp"

namespace duckdb {

ExtensionLoadResult ExtensionHelper::LoadExtension(DuckDB &db, const string &extension) {
	if (extension == "core_functions") {
		db.LoadStaticExtension<CoreFunctionsExtension>();
	} else if (extension == "json") {
		db.LoadStaticExtension<JsonExtension>();
	} else if (extension == "parquet") {
		db.LoadStaticExtension<ParquetExtension>();
	} else {
		return ExtensionLoadResult::NOT_LOADED;
	}
	return ExtensionLoadResult::LOADED_EXTENSION;
}

void ExtensionHelper::LoadAllExtensions(DuckDB &db) {
	for (const auto &name : {"core_functions", "json", "parquet"}) {
		LoadExtension(db, name);
	}
}

vector<string> ExtensionHelper::LoadedExtensionTestPaths() {
	return {};
}

} // namespace duckdb
