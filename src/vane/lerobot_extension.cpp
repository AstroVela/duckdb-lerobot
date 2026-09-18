#ifdef LEROBOT_VANE_DISTRIBUTED
#include "lerobot_extension.hpp"
#include "vane/lerobot_vane.hpp"
#include "function/lerobot_functions.hpp"
#include "duckdb/main/extension_helper.hpp"

namespace duckdb {
static void LoadVaneLerobot(ExtensionLoader &loader) {
	auto &instance = loader.GetDatabaseInstance();
	ExtensionHelper::AutoLoadExtension(instance, "parquet");
	ExtensionHelper::AutoLoadExtension(instance, "json");
	if (!instance.ExtensionIsLoaded("parquet") || !instance.ExtensionIsLoaded("json")) {
		throw MissingExtensionException("The lerobot extension requires parquet and json");
	}
	for (auto &functions : LerobotFunctions::GetTableFunctions()) {
		for (auto &function : functions.functions) {
			// TableFunctionSet(TableFunction) moves the name out of the overload.
			// Restore it before selecting adapters and binding capabilities.
			function.name = functions.name;
			ConfigureLerobotVaneScan(function);
			ConfigureLerobotVaneVideo(function);
			ConfigureLerobotVaneTemporal(function);
		}
		loader.RegisterFunction(std::move(functions));
	}
	loader.RegisterFunction(LerobotFunctions::GetDecodeImageFunction());
	RegisterLerobotVaneCopy(loader);
}
void LerobotExtension::Load(ExtensionLoader &loader) {
	LoadVaneLerobot(loader);
}
string LerobotExtension::Name() {
	return "lerobot";
}
} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(lerobot, loader) {
	duckdb::LoadVaneLerobot(loader);
}
}
#endif
