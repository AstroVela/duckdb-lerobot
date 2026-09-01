#include "function/lerobot_functions.hpp"

namespace duckdb {

vector<TableFunctionSet> LerobotFunctions::GetTableFunctions(ExtensionLoader &loader) {
	vector<TableFunctionSet> functions;
	functions.push_back(GetLayoutFunction());
	functions.push_back(GetV3ShardPathsFunction());
	functions.push_back(GetInfoFunction(loader));
	functions.push_back(GetEpisodesFunction(loader));
	functions.push_back(GetFramesFunction(loader));
	functions.push_back(GetMetadataCacheFunction());
	functions.push_back(GetEpisodeFramesFunction());
	return functions;
}

} // namespace duckdb
