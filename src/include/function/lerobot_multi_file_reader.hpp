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
	shared_ptr<MultiFileList> CreateFileList(ClientContext &context, const vector<string> &paths,
	                                         const FileGlobInput &glob_input) override;
	bool ParseOption(const string &key, const Value &val, MultiFileOptions &options, ClientContext &context) override;
	bool Bind(MultiFileOptions &options, MultiFileList &files, vector<LogicalType> &return_types, vector<string> &names,
	          MultiFileReaderBindData &bind_data) override;
	unique_ptr<MultiFileReader> Copy() const override;
	FileGlobInput GetGlobInput(MultiFileReaderInterface &interface) override;

private:
	LerobotScanKind kind;
	string root;
	vector<string> empty_names;
	vector<LogicalType> empty_types;
	bool empty_dataset = false;
	bool has_explicit_schema = false;
	bool file_row_number = false;
	bool binary_as_string = false;
	bool has_explicit_binary_as_string = false;
};

string NormalizeLerobotRoot(string root);

} // namespace duckdb
