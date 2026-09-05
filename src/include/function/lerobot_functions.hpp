//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_functions.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function_set.hpp"

namespace duckdb {

class LerobotFunctions {
public:
	static vector<TableFunctionSet> GetTableFunctions();
	static ScalarFunctionSet GetDecodeImageFunction();

private:
	static TableFunctionSet GetInfoFunction();
	static TableFunctionSet GetEpisodesFunction();
	static TableFunctionSet GetTasksFunction();
	static TableFunctionSet GetStatsFunction();
	static TableFunctionSet GetScanFunction();
	static TableFunctionSet GetCacheInfoFunction();
	static TableFunctionSet GetVideoRoutesFunction();
	static TableFunctionSet GetTemporalTargetsFunction();
	static TableFunctionSet GetVideoFramesFunction();
	static TableFunctionSet GetVideoWindowsFunction();
	static TableFunctionSet GetVideoTargetsFunction();
};

} // namespace duckdb
