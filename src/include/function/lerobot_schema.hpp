//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_schema.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

LogicalType LerobotDtypeToLogicalType(const string &dtype);
LogicalType LerobotFeatureStorageType(const string &dtype, const vector<idx_t> &shape);
LogicalType LerobotFeatureOutputType(const string &dtype, const vector<idx_t> &shape);
LogicalType LerobotFeatureScanType(const string &dtype, const vector<idx_t> &shape);
bool LerobotHasIntegerExtrema(const string &dtype);
LogicalType LerobotStatExtremaLeafType(const string &dtype);
LogicalType LerobotNestedListType(const LogicalType &leaf, idx_t dimensions);

} // namespace duckdb
