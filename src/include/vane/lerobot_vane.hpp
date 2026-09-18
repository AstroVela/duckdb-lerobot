#pragma once

#ifdef LEROBOT_VANE_DISTRIBUTED
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {
void ConfigureLerobotVaneScan(TableFunction &function);
void ConfigureLerobotVaneVideo(TableFunction &function);
void ConfigureLerobotVaneTemporal(TableFunction &function);
void RegisterLerobotVaneCopy(ExtensionLoader &loader);
} // namespace duckdb
#endif
