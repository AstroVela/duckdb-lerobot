//===----------------------------------------------------------------------===//
//                         DuckDB
//
// lerobot_path.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {

//! Normalize an explicit local or URI dataset root without guessing whether a
//! relative path is a Hugging Face repository identifier.
string NormalizeLerobotRoot(string root);

} // namespace duckdb
