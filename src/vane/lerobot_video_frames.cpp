#ifdef LEROBOT_VANE_DISTRIBUTED
#include "vane/lerobot_task_compat.hpp"
// Scope both API translations to this one native implementation.
#define ExecutorTask LerobotVaneProducerTask
#define ExecuteTask  ExecuteProducerTask
#define Reschedule() Reschedule(producer->CurrentInterruptEpoch())
#include "../function/video/lerobot_video_frames.cpp"
#undef Reschedule
#undef ExecuteTask
#undef ExecutorTask
#include "vane/lerobot_vane.hpp"
#include "vane/lerobot_snapshot.hpp"
#include "vane/lerobot_bind_scope.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/distributed_table_function.hpp"

namespace duckdb {
namespace {
struct VaneVideoBindData : TableFunctionData {
	unique_ptr<FunctionData> native;
	LerobotVaneVideoSnapshot video;
	string split_set;
	bool worker = false;
	bool assigned = false;
	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<VaneVideoBindData>();
		result->native = native->Copy();
		result->video = video;
		result->split_set = split_set;
		result->worker = worker;
		result->assigned = assigned;
		return std::move(result);
	}
};

unique_ptr<FunctionData> VaneVideoBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &types, vector<string> &names) {
	auto result = make_uniq<VaneVideoBindData>();
	const bool windows = input.table_function.name == "lerobot_video_windows";
	result->native = windows ? LerobotVideoWindowsBind(context, input, types, names)
	                         : LerobotVideoFramesBind(context, input, types, names);
	auto &native = result->native->Cast<LerobotVideoFramesBindData>();
	result->video = LerobotVaneVideoSnapshot::Capture(*native.metadata, native.routes);
	result->split_set = UUID::ToString(UUID::GenerateRandomUUID());
	if (!native.frame_queries.empty()) {
		// Partition by episode even when many episodes share one Parquet shard.
		// Windows retain the coordinator's original request ordinals.
		auto dataset = LerobotDatasetMetadata::Get(context, StringValue::Get(input.inputs[0]), false);
		vector<string> queries;
		if (windows) {
			map<int64_t, vector<LerobotVideoWindowRequest>> groups;
			for (const auto &request : GetVideoWindowRequests(input.inputs[1])) {
				groups[request.episode_index].push_back(request);
			}
			auto deltas =
			    GetLerobotTemporalDeltas(input, native.metadata->GetFPS(), native.tolerance, "lerobot_video_windows");
			for (const auto &group : groups) {
				auto route = dataset->FindEpisodeRoute(group.first);
				if (!route) {
					throw BinderException("LeRobot episode %d does not exist", group.first);
				}
				unordered_map<int64_t, int64_t> lengths {{group.first, route->episode_length}};
				queries.push_back(BuildVideoWindowQuery(
				    group.second, deltas, lengths, {dataset->GetDataFiles()[route->data_file_index]}, {group.first}));
			}
		} else {
			auto filter = input.named_parameters.find("frame_indices");
			for (auto episode : GetNonNegativeIndices(input.inputs[1], "episode_indices")) {
				auto route = dataset->FindEpisodeRoute(episode);
				if (!route) {
					continue;
				}
				auto query = "SELECT CAST(episode_index AS BIGINT), CAST(frame_index AS BIGINT), "
				             "CAST(timestamp AS DOUBLE) FROM read_parquet(" +
				             ValueListSQL({dataset->GetDataFiles()[route->data_file_index]}) +
				             ") WHERE episode_index = " + std::to_string(episode);
				if (filter != input.named_parameters.end()) {
					query +=
					    " AND frame_index IN " + IntegerListSQL(GetNonNegativeIndices(filter->second, "frame_indices"));
				}
				queries.push_back(std::move(query));
			}
		}
		native.frame_queries = std::move(queries);
	}
	return std::move(result);
}

void VaneVideoSerialize(Serializer &s, const optional_ptr<FunctionData> data, const TableFunction &) {
	auto &bind = data->Cast<VaneVideoBindData>();
	auto &native = bind.native->Cast<LerobotVideoFramesBindData>();
	s.WriteProperty(1, "split_set", bind.split_set);
	s.WriteProperty(2, "worker", bind.worker);
	s.WriteProperty(3, "assigned", bind.assigned);
	s.WriteProperty(4, "windows", native.window_mode);
	s.WriteObject(5, "video", [&](Serializer &o) { bind.video.Serialize(o); });
	s.WriteObject(6, "options", [&](Serializer &o) { LerobotVaneSerializeOptions(o, native.GetOptions()); });
	s.WriteProperty(7, "queries", native.frame_queries);
}
unique_ptr<FunctionData> VaneVideoDeserialize(Deserializer &d, TableFunction &) {
	auto bind = make_uniq<VaneVideoBindData>();
	bind->split_set = d.ReadProperty<string>(1, "split_set");
	bind->worker = d.ReadProperty<bool>(2, "worker");
	bind->assigned = d.ReadProperty<bool>(3, "assigned");
	auto windows = d.ReadProperty<bool>(4, "windows");
	d.ReadObject(5, "video", [&](Deserializer &o) { bind->video = LerobotVaneVideoSnapshot::Deserialize(o); });
	LerobotVideoOptions options;
	d.ReadObject(6, "options", [&](Deserializer &o) { options = LerobotVaneDeserializeOptions(o); });
	auto queries = d.ReadProperty<vector<string>>(7, "queries");
	if (bind->split_set.empty() || (bind->worker && !bind->assigned && !queries.empty())) {
		throw SerializationException("Invalid LeRobot video worker snapshot");
	}
	vector<LerobotVideoRoute> routes;
	auto metadata = bind->video.Restore(routes);
	bind->native = make_uniq<LerobotVideoFramesBindData>(std::move(metadata), std::move(routes), std::move(queries),
	                                                     windows, options);
	return std::move(bind);
}
vector<DistributedScanSplit> VaneVideoPlanSplits(const TableFunctionDistributedScanPlanningInput &input) {
	const auto &bind = input.bind_data->Cast<VaneVideoBindData>();
	const auto &native = bind.native->Cast<LerobotVideoFramesBindData>();
	if (bind.worker) {
		throw InternalException("Cannot plan splits from a LeRobot worker bind");
	}
	vector<DistributedScanSplit> splits;
	for (idx_t i = 0; i < native.frame_queries.size(); i++) {
		MemoryStream stream;
		BinarySerializer s(stream);
		s.Begin();
		s.WriteProperty(1, "split_set", bind.split_set);
		s.WriteProperty(2, "id", std::to_string(i));
		s.WriteProperty(3, "query", native.frame_queries[i]);
		s.End();
		DistributedScanSplit split;
		split.split_id = std::to_string(i);
		split.payload = string(reinterpret_cast<const char *>(stream.GetData()), stream.GetPosition());
		splits.push_back(std::move(split));
	}
	return splits;
}
unique_ptr<FunctionData> VaneVideoWorkerBind(const TableFunctionDistributedScanInput &input) {
	const auto &bind = input.bind_data->Cast<VaneVideoBindData>();
	auto copy = bind.Copy();
	auto &worker = copy->Cast<VaneVideoBindData>();
	worker.native->Cast<LerobotVideoFramesBindData>().frame_queries.clear();
	worker.worker = true;
	worker.assigned = false;
	return copy;
}
void VaneVideoApplySplits(optional_ptr<FunctionData> data, const vector<DistributedScanSplit> &splits) {
	auto &bind = data->Cast<VaneVideoBindData>();
	if (!bind.worker) {
		throw InvalidInputException("LeRobot split assignment requires a worker bind");
	}
	vector<string> queries;
	unordered_set<string> ids;
	for (const auto &split : splits) {
		MemoryStream stream(reinterpret_cast<data_ptr_t>(const_cast<char *>(split.payload.data())),
		                    split.payload.size());
		BinaryDeserializer d(stream);
		d.Begin();
		auto split_set = d.ReadProperty<string>(1, "split_set");
		auto id = d.ReadProperty<string>(2, "id");
		auto query = d.ReadProperty<string>(3, "query");
		d.End();
		if (split_set != bind.split_set || id != split.split_id || query.empty() || !ids.insert(id).second ||
		    stream.GetPosition() != split.payload.size()) {
			throw SerializationException("Invalid or duplicate LeRobot video split");
		}
		queries.push_back(std::move(query));
	}
	bind.native->Cast<LerobotVideoFramesBindData>().frame_queries = std::move(queries);
	bind.assigned = true;
}

struct VaneVideoTargetsBindData : TableFunctionData {
	unique_ptr<FunctionData> native;
	LerobotVaneDatasetSnapshot dataset;
	LerobotVaneVideoSnapshot video;
	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<VaneVideoTargetsBindData>();
		copy->native = native->Copy();
		copy->dataset = dataset;
		copy->video = video;
		return std::move(copy);
	}
};
unique_ptr<FunctionData> VaneVideoTargetsBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &types, vector<string> &names) {
	auto result = make_uniq<VaneVideoTargetsBindData>();
	result->native = LerobotVideoTargetsBind(context, input, types, names);
	auto &native = result->native->Cast<LerobotVideoTargetsBindData>();
	result->dataset = LerobotVaneDatasetSnapshot::Capture(context, *native.dataset);
	result->video = LerobotVaneVideoSnapshot::Capture(
	    *native.metadata, native.metadata->ResolveRoutes(result->dataset.episodes, native.metadata->GetVideoKeys()));
	return std::move(result);
}
void VaneVideoTargetsSerialize(Serializer &s, const optional_ptr<FunctionData> data, const TableFunction &) {
	auto &bind = data->Cast<VaneVideoTargetsBindData>();
	auto &native = bind.native->Cast<LerobotVideoTargetsBindData>();
	s.WriteObject(1, "dataset", [&](Serializer &o) { bind.dataset.Serialize(o); });
	s.WriteObject(2, "video", [&](Serializer &o) { bind.video.Serialize(o); });
	s.WriteObject(3, "options", [&](Serializer &o) { LerobotVaneSerializeOptions(o, native.options); });
	s.WriteObject(4, "deltas", [&](Serializer &o) { LerobotVaneSerializeDeltas(o, native.deltas); });
	s.WriteProperty(5, "input_columns", native.input_columns);
}
unique_ptr<FunctionData> VaneVideoTargetsDeserialize(Deserializer &d, TableFunction &) {
	auto bind = make_uniq<VaneVideoTargetsBindData>();
	d.ReadObject(1, "dataset", [&](Deserializer &o) { bind->dataset = LerobotVaneDatasetSnapshot::Deserialize(o); });
	d.ReadObject(2, "video", [&](Deserializer &o) { bind->video = LerobotVaneVideoSnapshot::Deserialize(o); });
	LerobotVideoOptions options;
	d.ReadObject(3, "options", [&](Deserializer &o) { options = LerobotVaneDeserializeOptions(o); });
	vector<LerobotTemporalDelta> deltas;
	d.ReadObject(4, "deltas", [&](Deserializer &o) { deltas = LerobotVaneDeserializeDeltas(o); });
	auto columns = d.ReadProperty<vector<idx_t>>(5, "input_columns");
	if (columns.size() != 5 && columns.size() != 6) {
		throw SerializationException("Invalid LeRobot video input columns");
	}
	vector<LerobotVideoRoute> routes;
	bind->native = make_uniq<LerobotVideoTargetsBindData>(bind->dataset.Restore(), bind->video.Restore(routes),
	                                                      std::move(deltas), std::move(columns), options);
	return std::move(bind);
}
} // namespace

void ConfigureLerobotVaneVideo(TableFunction &function) {
	if (function.name == "lerobot_video_frames" || function.name == "lerobot_video_windows") {
		function.bind = VaneVideoBind;
		function.serialize = VaneVideoSerialize;
		function.deserialize = VaneVideoDeserialize;
		function.init_global = [](ClientContext &context, TableFunctionInitInput &input) {
			auto &bind = input.bind_data->Cast<VaneVideoBindData>();
			if (bind.worker && !bind.assigned) {
				throw InvalidInputException("LeRobot video worker has no split assignment");
			}
			LerobotVaneBindScope<TableFunctionInitInput> scope(input, *bind.native);
			return LerobotVideoFramesInitGlobal(context, input);
		};
		function.init_local = [](ExecutionContext &context, TableFunctionInitInput &input,
		                         GlobalTableFunctionState *state) {
			LerobotVaneBindScope<TableFunctionInitInput> scope(input,
			                                                   *input.bind_data->Cast<VaneVideoBindData>().native);
			return LerobotVideoFramesInitLocal(context, input, state);
		};
		function.function = [](ClientContext &context, TableFunctionInput &input, DataChunk &out) {
			LerobotVaneBindScope<TableFunctionInput> scope(input, *input.bind_data->Cast<VaneVideoBindData>().native);
			LerobotVideoFramesFunction(context, input, out);
		};
		TableFunctionDistributedScanCallbacks callbacks;
		callbacks.protocol_version = 1;
		callbacks.split_codec = {"lerobot.video-episode", 1};
		callbacks.plan_splits = VaneVideoPlanSplits;
		callbacks.create_worker_bind = VaneVideoWorkerBind;
		callbacks.apply_splits = VaneVideoApplySplits;
		function.SetDistributedScanCallbacks(std::move(callbacks));
	} else if (function.name == "lerobot_video_targets") {
		function.bind = VaneVideoTargetsBind;
		function.serialize = VaneVideoTargetsSerialize;
		function.deserialize = VaneVideoTargetsDeserialize;
		function.init_global = [](ClientContext &context, TableFunctionInitInput &input) {
			LerobotVaneBindScope<TableFunctionInitInput> scope(
			    input, *input.bind_data->Cast<VaneVideoTargetsBindData>().native);
			return LerobotVideoTargetsInitGlobal(context, input);
		};
		function.in_out_function = [](ExecutionContext &context, TableFunctionInput &input, DataChunk &in,
		                              DataChunk &out) {
			LerobotVaneBindScope<TableFunctionInput> scope(input,
			                                               *input.bind_data->Cast<VaneVideoTargetsBindData>().native);
			return LerobotVideoTargetsFunction(context, input, in, out);
		};
	}
}
} // namespace duckdb
#endif
