//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_copy.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/copy_function.hpp"

namespace duckdb {

struct LerobotCopyFunction {
	static CopyFunction Create();
};

} // namespace duckdb
