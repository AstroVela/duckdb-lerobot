#include "function/lerobot_functions.hpp"

namespace duckdb {

vector<TableFunctionSet> LerobotFunctions::GetTableFunctions() {
	vector<TableFunctionSet> functions;
	functions.push_back(GetInfoFunction());
	functions.push_back(GetEpisodesFunction());
	functions.push_back(GetTasksFunction());
	functions.push_back(GetStatsFunction());
	functions.push_back(GetScanFunction());
	functions.push_back(GetCacheInfoFunction());
	functions.push_back(GetVideoRoutesFunction());
	functions.push_back(GetTemporalTargetsFunction());
	functions.push_back(GetVideoFramesFunction());
	functions.push_back(GetVideoWindowsFunction());
	functions.push_back(GetVideoTargetsFunction());
	return functions;
}

} // namespace duckdb
