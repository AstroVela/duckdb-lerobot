#ifdef LEROBOT_VANE_DISTRIBUTED
#include "../function/temporal/lerobot_temporal_targets.cpp"
#include "vane/lerobot_vane.hpp"
#include "vane/lerobot_snapshot.hpp"
#include "vane/lerobot_bind_scope.hpp"

namespace duckdb {
namespace {
struct VaneTemporalBindData : TableFunctionData {
	unique_ptr<FunctionData> native;
	LerobotVaneDatasetSnapshot dataset;
	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<VaneTemporalBindData>();
		result->native = native->Copy();
		result->dataset = dataset;
		return std::move(result);
	}
};
unique_ptr<FunctionData> VaneTemporalBind(ClientContext &context, TableFunctionBindInput &input,
                                          vector<LogicalType> &types, vector<string> &names) {
	auto result = make_uniq<VaneTemporalBindData>();
	result->native = LerobotTemporalTargetsBind(context, input, types, names);
	result->dataset =
	    LerobotVaneDatasetSnapshot::Capture(context, *result->native->Cast<LerobotTemporalTargetsBindData>().metadata);
	return std::move(result);
}
void VaneTemporalSerialize(Serializer &s, const optional_ptr<FunctionData> data, const TableFunction &) {
	auto &bind = data->Cast<VaneTemporalBindData>();
	auto &native = bind.native->Cast<LerobotTemporalTargetsBindData>();
	s.WriteObject(1, "dataset", [&](Serializer &o) { bind.dataset.Serialize(o); });
	s.WriteObject(2, "deltas", [&](Serializer &o) { LerobotVaneSerializeDeltas(o, native.deltas); });
	s.WriteProperty(3, "input_columns", native.input_columns);
}
unique_ptr<FunctionData> VaneTemporalDeserialize(Deserializer &d, TableFunction &) {
	auto result = make_uniq<VaneTemporalBindData>();
	d.ReadObject(1, "dataset", [&](Deserializer &o) { result->dataset = LerobotVaneDatasetSnapshot::Deserialize(o); });
	vector<LerobotTemporalDelta> deltas;
	d.ReadObject(2, "deltas", [&](Deserializer &o) { deltas = LerobotVaneDeserializeDeltas(o); });
	auto columns = d.ReadProperty<vector<idx_t>>(3, "input_columns");
	if (columns.size() != 4 && columns.size() != 5) {
		throw SerializationException("Invalid LeRobot temporal input columns");
	}
	result->native =
	    make_uniq<LerobotTemporalTargetsBindData>(result->dataset.Restore(), std::move(deltas), std::move(columns));
	return std::move(result);
}
} // namespace

void ConfigureLerobotVaneTemporal(TableFunction &function) {
	if (function.name != "lerobot_temporal_targets") {
		return;
	}
	function.bind = VaneTemporalBind;
	function.serialize = VaneTemporalSerialize;
	function.deserialize = VaneTemporalDeserialize;
	function.init_global = [](ClientContext &context, TableFunctionInitInput &input) {
		LerobotVaneBindScope<TableFunctionInitInput> scope(input,
		                                                   *input.bind_data->Cast<VaneTemporalBindData>().native);
		return LerobotTemporalTargetsInitGlobal(context, input);
	};
	function.in_out_function = [](ExecutionContext &context, TableFunctionInput &input, DataChunk &in, DataChunk &out) {
		LerobotVaneBindScope<TableFunctionInput> scope(input, *input.bind_data->Cast<VaneTemporalBindData>().native);
		return LerobotTemporalTargetsFunction(context, input, in, out);
	};
}
} // namespace duckdb
#endif
