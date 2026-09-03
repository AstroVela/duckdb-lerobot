#include "function/lerobot_functions.hpp"

namespace duckdb {

vector<TableFunctionSet> LerobotFunctions::GetTableFunctions(ExtensionLoader &loader) {
	vector<TableFunctionSet> functions;
	functions.push_back(GetLayoutFunction());
	functions.push_back(GetV3ShardPathsFunction());
	functions.push_back(GetInfoFunction(loader));
	functions.push_back(GetEpisodesFunction(loader));
	functions.push_back(GetTasksFunction(loader));
	functions.push_back(GetFramesFunction());
	functions.push_back(GetMetadataCacheFunction());
	functions.push_back(GetVideoMetadataCacheFunction());
	functions.push_back(GetVideoRoutesFunction());
	functions.push_back(GetEpisodeFramesFunction());
	functions.push_back(GetTemporalTargetsFunction());
	functions.push_back(GetVideoFramesFunction());
	functions.push_back(GetVideoWindowsFunction());
	functions.push_back(GetVideoTargetsFunction());
	return functions;
}

} // namespace duckdb
