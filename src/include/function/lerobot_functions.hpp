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
	static TableFunctionSet GetTasksFunction(ExtensionLoader &loader);
	static TableFunctionSet GetFramesFunction(ExtensionLoader &loader);
	static TableFunctionSet GetMetadataCacheFunction();
	static TableFunctionSet GetVideoMetadataCacheFunction();
	static TableFunctionSet GetVideoRoutesFunction();
	static TableFunctionSet GetEpisodeFramesFunction();
	static TableFunctionSet GetTemporalTargetsFunction();
	static TableFunctionSet GetVideoFramesFunction();
	static TableFunctionSet GetVideoWindowsFunction();
	static TableFunctionSet GetVideoTargetsFunction();
};

} // namespace duckdb
