//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_multi_file_reader.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_reader.hpp"

namespace duckdb {

enum class LerobotScanKind : uint8_t { INFO, EPISODES, TASKS, FRAMES };

class LerobotMultiFileReader final : public MultiFileReader {
public:
	explicit LerobotMultiFileReader(LerobotScanKind kind_p);

	static unique_ptr<MultiFileReader> CreateInfo(const TableFunction &function);
	static unique_ptr<MultiFileReader> CreateEpisodes(const TableFunction &function);
	static unique_ptr<MultiFileReader> CreateTasks(const TableFunction &function);
	static unique_ptr<MultiFileReader> CreateFrames(const TableFunction &function);

	vector<string> ParsePaths(const Value &input) override;
	unique_ptr<MultiFileReader> Copy() const override;

private:
	LerobotScanKind kind;
};

string NormalizeLerobotRoot(string root);

} // namespace duckdb
