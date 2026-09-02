#include "lerobot_extension.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "function/lerobot_functions.hpp"
#include "function/lerobot_copy.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	auto &instance = loader.GetDatabaseInstance();
	ExtensionHelper::AutoLoadExtension(instance, "parquet");
	ExtensionHelper::AutoLoadExtension(instance, "json");

	if (!instance.ExtensionIsLoaded("parquet")) {
		throw MissingExtensionException("The lerobot extension requires the parquet extension to be loaded");
	}
	if (!instance.ExtensionIsLoaded("json")) {
		throw MissingExtensionException("The lerobot extension requires the json extension to be loaded");
	}

	for (auto &function : LerobotFunctions::GetTableFunctions(loader)) {
		loader.RegisterFunction(std::move(function));
	}
	loader.RegisterFunction(LerobotCopyFunction::Create());
}

void LerobotExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

string LerobotExtension::Name() {
	return "lerobot";
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(lerobot, loader) {
	duckdb::LoadInternal(loader);
}
}
