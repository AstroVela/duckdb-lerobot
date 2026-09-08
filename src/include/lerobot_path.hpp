//===----------------------------------------------------------------------===//
//                         DuckDB
//
// lerobot_path.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {

class ClientContext;

//! Normalize an explicit local or URI dataset root without guessing whether a
//! relative path is a Hugging Face repository identifier.
string NormalizeLerobotRoot(string root);

//! Resolve local roots in the caller's file_search_path before nested queries
//! and direct filesystem reads. Explicit absolute paths and URIs stay explicit.
string ResolveLerobotRoot(ClientContext &context, const string &root);

//! Cache identity includes local search paths and a SHA-256 fingerprint of S3
//! access settings and secrets overlapping any path in the dataset. Does not
//! inspect dataset files or retain plaintext credentials in the key.
string LerobotRootCacheKey(ClientContext &context, const string &root);

//! Resolve an expanded metadata template made of portable relative components.
//! The dataset root keeps its filesystem/URI semantics; templates cannot add
//! traversal, URI delimiters or glob expressions.
string ResolveLerobotRelativePath(const string &root, const string &relative_path, const char *path_name);

//! Feature names are used as path components by the video reader and writer.
void ValidateLerobotFeatureName(const string &name);

} // namespace duckdb
