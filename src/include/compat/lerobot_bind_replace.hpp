//===----------------------------------------------------------------------===//
//                         DuckDB
//
// compat/lerobot_bind_replace.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

//! Build native table-function relations without constructing SQL strings.
//! These helpers isolate DuckDB parsed-AST APIs used by bind replacement.
unique_ptr<TableFunctionRef>
LerobotCreateTableFunctionRef(const char *name, Value argument,
                              const named_parameter_map_t &named_parameters = named_parameter_map_t());

Value LerobotCreatePathList(const vector<string> &paths);

//! Produce a typed zero-row relation and mirror the documented Parquet output
//! options that affect an empty scan's schema.
unique_ptr<TableRef>
LerobotCreateEmptyParquetRelation(ClientContext &context, vector<string> names, vector<LogicalType> types,
                                  const named_parameter_map_t &parameters = named_parameter_map_t());

} // namespace duckdb
