#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

class ClientContext;
class DataChunk;
class QueryResult;

//! Copy explicit session overrides for extension options (e.g. HTTPFS
//! credentials and endpoints) into a fresh reader on the same database,
//! before it starts querying. Global settings and secrets remain shared.
//! This does not copy transactions, profiling or arbitrary session state.
void InheritLerobotReaderSettings(ClientContext &parent, ClientContext &reader);

//! A nested read with cancellation inherited from its caller. Keep the result
//! and connection together: cancellation registration outlives result cleanup,
//! and is removed before destroying the connection. Execution and cleanup stay
//! synchronous with the caller; the shared monitor only forwards Interrupt().
class LerobotNestedQuery {
public:
	LerobotNestedQuery(ClientContext &context, const string &sql, bool stream = false);
	~LerobotNestedQuery();

	const QueryResult &GetResult() const;
	unique_ptr<DataChunk> Fetch();

private:
	struct Impl;
	unique_ptr<Impl> impl;
};

} // namespace duckdb
