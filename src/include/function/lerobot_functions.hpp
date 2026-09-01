//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_functions.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function_set.hpp"

namespace duckdb {

class ExtensionLoader;

class LerobotFunctions {
public:
	static vector<TableFunctionSet> GetTableFunctions(ExtensionLoader &loader);

private:
	static TableFunctionSet GetLayoutFunction();
	static TableFunctionSet GetV3ShardPathsFunction();
	static TableFunctionSet GetInfoFunction(ExtensionLoader &loader);
	static TableFunctionSet GetEpisodesFunction(ExtensionLoader &loader);
	static TableFunctionSet GetFramesFunction(ExtensionLoader &loader);
	static TableFunctionSet GetMetadataCacheFunction();
	static TableFunctionSet GetEpisodeFramesFunction();
};

} // namespace duckdb
