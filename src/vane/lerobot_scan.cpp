#ifdef LEROBOT_VANE_DISTRIBUTED
// Compile the native implementation once, alongside its Vane adapter. Keeping
// this in one translation unit lets the adapter use its private bind types.
#include "../function/scan/lerobot_scan.cpp"
#include "vane/lerobot_vane.hpp"
#include "vane/lerobot_snapshot.hpp"
#include "duckdb/function/distributed_table_function.hpp"

namespace duckdb {
namespace {
struct VaneCacheSnapshot : TableFunctionData {
	vector<vector<Value>> rows;
	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<VaneCacheSnapshot>();
		copy->rows = rows;
		return std::move(copy);
	}
};
unique_ptr<FunctionData> VaneCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &types, vector<string> &names) {
	auto native = LerobotCacheInfoBind(context, input, types, names);
	LerobotCacheInfoGlobalState state;
	TableFunctionInput scan(native.get(), nullptr, &state);
	DataChunk chunk;
	chunk.Initialize(Allocator::Get(context), types);
	LerobotCacheInfoFunction(context, scan, chunk);
	auto result = make_uniq<VaneCacheSnapshot>();
	for (idx_t row = 0; row < chunk.size(); row++) {
		vector<Value> values;
		for (idx_t col = 0; col < chunk.ColumnCount(); col++) {
			values.push_back(chunk.GetValue(col, row));
		}
		result->rows.push_back(std::move(values));
	}
	return std::move(result);
}
void VaneCacheScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	const auto &rows = input.bind_data->Cast<VaneCacheSnapshot>().rows;
	auto &state = input.global_state->Cast<LerobotCacheInfoGlobalState>();
	idx_t count = 0;
	while (state.next_component < rows.size() && count < STANDARD_VECTOR_SIZE) {
		for (idx_t col = 0; col < output.ColumnCount(); col++) {
			output.SetValue(col, count, rows[state.next_component][col]);
		}
		state.next_component++;
		count++;
	}
	output.SetCardinality(count);
}
} // namespace

void ConfigureLerobotVaneScan(TableFunction &function) {
	if (function.name == "lerobot_video_routes") {
		function.serialize = [](Serializer &s, const optional_ptr<FunctionData> data, const TableFunction &) {
			auto &bind = data->Cast<LerobotVideoRoutesBindData>();
			LerobotVaneVideoSnapshot::Capture(*bind.metadata, bind.routes).Serialize(s);
		};
		function.deserialize = [](Deserializer &d, TableFunction &) -> unique_ptr<FunctionData> {
			auto snapshot = LerobotVaneVideoSnapshot::Deserialize(d);
			vector<LerobotVideoRoute> routes;
			auto metadata = snapshot.Restore(routes);
			return make_uniq<LerobotVideoRoutesBindData>(std::move(metadata), std::move(routes));
		};
		function.SetDistributedScanCallbacks(MakeDistributedSingletonSourceCallbacks());
	} else if (function.name == "lerobot_cache_info") {
		function.bind = VaneCacheBind;
		function.function = VaneCacheScan;
		function.serialize = [](Serializer &s, const optional_ptr<FunctionData> data, const TableFunction &) {
			s.WriteProperty(1, "rows", data->Cast<VaneCacheSnapshot>().rows);
		};
		function.deserialize = [](Deserializer &d, TableFunction &) -> unique_ptr<FunctionData> {
			auto result = make_uniq<VaneCacheSnapshot>();
			result->rows = d.ReadProperty<vector<vector<Value>>>(1, "rows");
			if (result->rows.size() != 2 || result->rows[0].size() != 5 || result->rows[1].size() != 5) {
				throw SerializationException("Invalid LeRobot cache snapshot");
			}
			return std::move(result);
		};
		function.SetDistributedScanCallbacks(MakeDistributedSingletonSourceCallbacks());
	}
}
} // namespace duckdb
#endif
