#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/unique_ptr.hpp"

#include <utility>

namespace duckdb {

class ClientContext;

using LerobotTimestampKey = std::pair<int64_t, int64_t>;

struct LerobotTimestampMatch {
	int64_t count = 0;
	double timestamp = 0;
	bool has_timestamp = false;
};

struct LerobotTimestampLookupStats {
	idx_t queries;
	idx_t index_loads;
	idx_t index_hits;
	idx_t evictions;
	idx_t indexed_rows;
	idx_t bytes;
	idx_t peak_bytes;
};

//! Shared only by workers of one video_targets operator. Repeated shard reads
//! can build a bounded index; one-off, oversized or contended loads retain the
//! native filtered query. No entries or borrowed contexts survive the query.
class LerobotTimestampLookup {
public:
	explicit LerobotTimestampLookup(ClientContext &context);
	//! Internal constructor for resource-bound tests; not a SQL execution knob.
	LerobotTimestampLookup(ClientContext &context, idx_t max_bytes, idx_t min_requests = 0);
	~LerobotTimestampLookup();

	//! Keys must be sorted and unique. Results have the same order. Counts are
	//! combined across *all* selected files, including misplaced duplicate rows.
	vector<LerobotTimestampMatch> Lookup(ClientContext &context, const vector<string> &files,
	                                     const vector<LerobotTimestampKey> &keys, idx_t request_count = 0);
	LerobotTimestampLookupStats GetStats() const;

private:
	struct Impl;
	unique_ptr<Impl> impl;
};

} // namespace duckdb
