#pragma once

#ifdef LEROBOT_VANE_DISTRIBUTED
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "storage/lerobot_metadata_cache.hpp"
#include "function/lerobot_temporal.hpp"
#include "function/lerobot_video_options.hpp"

namespace duckdb {
// Only immutable values cross a process boundary. Snapshots never enter the
// native object cache and never reopen mutable metadata on a worker.
struct LerobotVaneDatasetSnapshot {
	string root;
	int64_t fps = 0;
	vector<string> files;
	vector<int64_t> episodes;
	vector<int64_t> lengths;
	vector<idx_t> file_ids;
	static LerobotVaneDatasetSnapshot Capture(ClientContext &context, const LerobotDatasetMetadata &metadata);
	shared_ptr<LerobotDatasetMetadata> Restore() const;
	void Serialize(Serializer &serializer) const;
	static LerobotVaneDatasetSnapshot Deserialize(Deserializer &deserializer);
};

struct LerobotVaneVideoSnapshot {
	int64_t fps = 0;
	vector<Value> routes;
	static LerobotVaneVideoSnapshot Capture(const LerobotVideoMetadata &metadata,
	                                        const vector<LerobotVideoRoute> &routes);
	shared_ptr<LerobotVideoMetadata> Restore(vector<LerobotVideoRoute> &routes) const;
	void Serialize(Serializer &serializer) const;
	static LerobotVaneVideoSnapshot Deserialize(Deserializer &deserializer);
};

void LerobotVaneSerializeOptions(Serializer &serializer, const LerobotVideoOptions &options);
LerobotVideoOptions LerobotVaneDeserializeOptions(Deserializer &deserializer);
void LerobotVaneSerializeDeltas(Serializer &serializer, const vector<LerobotTemporalDelta> &deltas);
vector<LerobotTemporalDelta> LerobotVaneDeserializeDeltas(Deserializer &deserializer);
} // namespace duckdb
#endif
