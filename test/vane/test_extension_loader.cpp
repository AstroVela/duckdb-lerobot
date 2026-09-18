#ifdef LEROBOT_VANE_DISTRIBUTED
#include "../cpp/test_extension_loader.cpp"

namespace duckdb {
bool ExtensionHelper::IsExtensionLinked(const string &extension) {
	return extension == "core_functions" || extension == "json" || extension == "parquet";
}
} // namespace duckdb
#endif
