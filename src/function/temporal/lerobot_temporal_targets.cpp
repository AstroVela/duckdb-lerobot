#include "function/lerobot_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"

#include "function/lerobot_temporal.hpp"
#include "lerobot_path.hpp"
#include "storage/lerobot_metadata_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <utility>

namespace duckdb {

namespace {

int64_t RoundHalfToEven(double value) {
	const auto lower = std::floor(value);
	const auto fraction = value - lower;
	if (fraction < 0.5) {
		return static_cast<int64_t>(lower);
	}
	if (fraction > 0.5) {
		return static_cast<int64_t>(lower + 1);
	}
	return static_cast<int64_t>(std::fmod(lower, 2.0) == 0.0 ? lower : lower + 1);
}

} // namespace

vector<LerobotTemporalDelta> GetLerobotTemporalDeltas(TableFunctionBindInput &input, int64_t fps, double tolerance,
                                                      const char *function_name) {
	vector<double> timestamps;
	auto entry = input.named_parameters.find("delta_timestamps");
	if (entry == input.named_parameters.end()) {
		timestamps.push_back(0);
	} else {
		if (entry->second.IsNull()) {
			throw BinderException("%s delta_timestamps must not be NULL", function_name);
		}
		for (const auto &child : ListValue::GetChildren(entry->second)) {
			if (child.IsNull()) {
				throw BinderException("%s delta_timestamps must not contain NULL", function_name);
			}
			timestamps.push_back(child.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>());
		}
	}

	vector<LerobotTemporalDelta> result;
	result.reserve(timestamps.size());
	for (const auto timestamp : timestamps) {
		if (!std::isfinite(timestamp)) {
			throw BinderException("%s delta_timestamps must be finite", function_name);
		}
		// Python round(), used by native LeRobot, resolves exact half ties to the
		// even integer. Spell the operation out instead of relying on the process
		// floating-point rounding mode.
		const auto scaled = timestamp * static_cast<double>(fps);
		const auto int64_upper_bound = std::ldexp(1.0, 63);
		if (!std::isfinite(scaled) || scaled < -int64_upper_bound || scaled >= int64_upper_bound) {
			throw BinderException("%s delta timestamp %.17g is too large", function_name, timestamp);
		}
		const auto frame_offset = RoundHalfToEven(scaled);
		const auto canonical_timestamp = static_cast<double>(frame_offset) / static_cast<double>(fps);
		if (std::fabs(timestamp - canonical_timestamp) > tolerance) {
			throw BinderException("%s delta timestamp %.17g is not a multiple of 1/fps (%d) within tolerance %.17g",
			                      function_name, timestamp, fps, tolerance);
		}
		LerobotTemporalDelta delta;
		delta.timestamp = timestamp;
		delta.frame_offset = frame_offset;
		result.push_back(delta);
	}
	return result;
}

namespace {

template <typename T>
T *GetMutableFlatData(Vector &vector) {
	return FlatVector::GetData<T>(vector);
}

void PrepareUnifiedFormat(Vector &vector, idx_t count, UnifiedVectorFormat &format) {
	vector.ToUnifiedFormat(count, format);
}

void SetOutputCardinality(DataChunk &output, idx_t count) {
	output.SetCardinality(count);
}

struct LerobotTemporalTargetsBindData final : public TableFunctionData {
	LerobotTemporalTargetsBindData(shared_ptr<LerobotDatasetMetadata> metadata_p, vector<LerobotTemporalDelta> deltas_p,
	                               vector<idx_t> input_columns_p)
	    : metadata(std::move(metadata_p)), deltas(std::move(deltas_p)), input_columns(std::move(input_columns_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotTemporalTargetsBindData>(metadata, deltas, input_columns);
	}

	shared_ptr<LerobotDatasetMetadata> metadata;
	vector<LerobotTemporalDelta> deltas;
	//! Physical input indexes in request_id, episode_index, frame_index,
	//! delta_index, optional target_id order.
	vector<idx_t> input_columns;
};

bool GetRefreshParameter(TableFunctionBindInput &input) {
	auto entry = input.named_parameters.find("refresh");
	if (entry == input.named_parameters.end()) {
		return false;
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_temporal_targets refresh must not be NULL");
	}
	return BooleanValue::Get(entry->second);
}

double GetToleranceParameter(TableFunctionBindInput &input) {
	auto entry = input.named_parameters.find("tolerance");
	if (entry == input.named_parameters.end()) {
		return LEROBOT_DEFAULT_TEMPORAL_TOLERANCE_SECONDS;
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_temporal_targets tolerance must not be NULL");
	}
	auto tolerance = entry->second.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
	if (!std::isfinite(tolerance) || tolerance <= 0) {
		throw BinderException("lerobot_temporal_targets tolerance must be finite and positive");
	}
	return tolerance;
}

idx_t FindInputColumn(TableFunctionBindInput &input, const char *name) {
	optional_idx result;
	for (idx_t column = 0; column < input.input_table_names.size(); column++) {
		if (input.input_table_names[column] != name) {
			continue;
		}
		if (result.IsValid()) {
			throw BinderException("lerobot_temporal_targets input relation contains duplicate column '%s'", name);
		}
		result = column;
	}
	if (!result.IsValid()) {
		throw BinderException("lerobot_temporal_targets input relation requires column '%s'", name);
	}
	return result.GetIndex();
}

unique_ptr<FunctionData> LerobotTemporalTargetsBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw BinderException("lerobot_temporal_targets root must not be NULL");
	}
	if ((input.input_table_types.size() != 4 && input.input_table_types.size() != 5) ||
	    input.input_table_types.size() != input.input_table_names.size()) {
		throw BinderException("lerobot_temporal_targets input relation must contain exactly request_id, episode_index, "
		                      "frame_index, and delta_index, with an optional target_id");
	}

	vector<idx_t> input_columns;
	input_columns.push_back(FindInputColumn(input, "request_id"));
	input_columns.push_back(FindInputColumn(input, "episode_index"));
	input_columns.push_back(FindInputColumn(input, "frame_index"));
	input_columns.push_back(FindInputColumn(input, "delta_index"));
	if (input.input_table_types.size() == 5) {
		if (std::find(input.input_table_names.begin(), input.input_table_names.end(), "target_id") ==
		    input.input_table_names.end()) {
			throw BinderException(
			    "lerobot_temporal_targets input relation must contain exactly request_id, episode_index, "
			    "frame_index, and delta_index, with an optional target_id");
		}
		input_columns.push_back(FindInputColumn(input, "target_id"));
	}
	for (const auto column : input_columns) {
		input.input_table_types[column] = LogicalType::BIGINT;
	}

	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	auto metadata = LerobotDatasetMetadata::Get(context, root, GetRefreshParameter(input));
	auto deltas =
	    GetLerobotTemporalDeltas(input, metadata->GetFPS(), GetToleranceParameter(input), "lerobot_temporal_targets");

	names = {"request_id",      "target_ordinal",     "episode_index", "frame_index",       "delta_index",
	         "delta_timestamp", "delta_frame_offset", "is_padding",    "target_frame_index"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT,  LogicalType::BIGINT,
	                LogicalType::BIGINT, LogicalType::BIGINT,  LogicalType::DOUBLE,
	                LogicalType::BIGINT, LogicalType::BOOLEAN, LogicalType::BIGINT};
	if (input_columns.size() == 5) {
		names.push_back("target_id");
		return_types.push_back(LogicalType::BIGINT);
	}
	return make_uniq<LerobotTemporalTargetsBindData>(std::move(metadata), std::move(deltas), std::move(input_columns));
}

enum LerobotTemporalTargetColumn {
	LEROBOT_TEMPORAL_REQUEST_ID = 0,
	LEROBOT_TEMPORAL_TARGET_ORDINAL = 1,
	LEROBOT_TEMPORAL_EPISODE_INDEX = 2,
	LEROBOT_TEMPORAL_FRAME_INDEX = 3,
	LEROBOT_TEMPORAL_DELTA_INDEX = 4,
	LEROBOT_TEMPORAL_DELTA_TIMESTAMP = 5,
	LEROBOT_TEMPORAL_DELTA_FRAME_OFFSET = 6,
	LEROBOT_TEMPORAL_IS_PADDING = 7,
	LEROBOT_TEMPORAL_TARGET_FRAME_INDEX = 8,
	LEROBOT_TEMPORAL_TARGET_ID = 9,
	LEROBOT_TEMPORAL_COLUMN_COUNT = 10
};

struct LerobotTemporalTargetsGlobalState final : public GlobalTableFunctionState {
	explicit LerobotTemporalTargetsGlobalState(const vector<column_t> &column_ids) : next_target_ordinal(0) {
		for (const auto column_id : column_ids) {
			const auto logical_column = static_cast<idx_t>(column_id);
			if (logical_column >= LEROBOT_TEMPORAL_COLUMN_COUNT) {
				throw InternalException("Invalid projected column for lerobot_temporal_targets");
			}
			projected_columns.push_back(logical_column);
		}
	}

	idx_t MaxThreads() const override {
		return GlobalTableFunctionState::MAX_THREADS;
	}

	uint64_t ClaimTargetOrdinals(idx_t count) {
		return next_target_ordinal.fetch_add(static_cast<uint64_t>(count), std::memory_order_relaxed);
	}

	std::atomic<uint64_t> next_target_ordinal;
	vector<idx_t> projected_columns;
};

unique_ptr<GlobalTableFunctionState> LerobotTemporalTargetsInitGlobal(ClientContext &, TableFunctionInitInput &input) {
	return make_uniq<LerobotTemporalTargetsGlobalState>(input.column_ids);
}

int64_t ReadInputInteger(const vector<UnifiedVectorFormat> &formats, idx_t column, idx_t row, const char *column_name) {
	const auto &format = formats[column];
	const auto source_index = format.sel->get_index(row);
	if (!format.validity.RowIsValid(source_index)) {
		throw InvalidInputException("lerobot_temporal_targets input column '%s' must not contain NULL", column_name);
	}
	if (format.physical_type != PhysicalType::INT64) {
		throw InternalException("Expected BIGINT vector for lerobot_temporal_targets column '%s'", column_name);
	}
	return format.GetData<int64_t>()[source_index];
}

struct LerobotTemporalTarget {
	int64_t request_id;
	int64_t target_ordinal;
	int64_t target_id;
	int64_t episode_index;
	int64_t frame_index;
	int64_t delta_index;
	double delta_timestamp;
	int64_t delta_frame_offset;
	bool is_padding;
	int64_t target_frame_index;
};

void WriteTargetColumn(const LerobotTemporalTarget &target, idx_t logical_column, idx_t row, Vector &output) {
	switch (logical_column) {
	case LEROBOT_TEMPORAL_REQUEST_ID:
		GetMutableFlatData<int64_t>(output)[row] = target.request_id;
		break;
	case LEROBOT_TEMPORAL_TARGET_ORDINAL:
		GetMutableFlatData<int64_t>(output)[row] = target.target_ordinal;
		break;
	case LEROBOT_TEMPORAL_TARGET_ID:
		GetMutableFlatData<int64_t>(output)[row] = target.target_id;
		break;
	case LEROBOT_TEMPORAL_EPISODE_INDEX:
		GetMutableFlatData<int64_t>(output)[row] = target.episode_index;
		break;
	case LEROBOT_TEMPORAL_FRAME_INDEX:
		GetMutableFlatData<int64_t>(output)[row] = target.frame_index;
		break;
	case LEROBOT_TEMPORAL_DELTA_INDEX:
		GetMutableFlatData<int64_t>(output)[row] = target.delta_index;
		break;
	case LEROBOT_TEMPORAL_DELTA_TIMESTAMP:
		GetMutableFlatData<double>(output)[row] = target.delta_timestamp;
		break;
	case LEROBOT_TEMPORAL_DELTA_FRAME_OFFSET:
		GetMutableFlatData<int64_t>(output)[row] = target.delta_frame_offset;
		break;
	case LEROBOT_TEMPORAL_IS_PADDING:
		GetMutableFlatData<bool>(output)[row] = target.is_padding;
		break;
	case LEROBOT_TEMPORAL_TARGET_FRAME_INDEX:
		GetMutableFlatData<int64_t>(output)[row] = target.target_frame_index;
		break;
	default:
		throw InternalException("Invalid lerobot_temporal_targets projected column");
	}
}

OperatorResultType LerobotTemporalTargetsFunction(ExecutionContext &, TableFunctionInput &input,
                                                  DataChunk &target_input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotTemporalTargetsBindData>();
	auto &global_state = input.global_state->Cast<LerobotTemporalTargetsGlobalState>();

	vector<UnifiedVectorFormat> formats;
	formats.reserve(target_input.ColumnCount());
	for (idx_t column = 0; column < target_input.ColumnCount(); column++) {
		formats.push_back(UnifiedVectorFormat());
		PrepareUnifiedFormat(target_input.data[column], target_input.size(), formats.back());
	}

	const auto first_ordinal = global_state.ClaimTargetOrdinals(target_input.size());
	if (first_ordinal > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - target_input.size()) {
		throw InvalidInputException("lerobot_temporal_targets produced too many rows for target_ordinal");
	}
	const char *input_names[] = {"request_id", "episode_index", "frame_index", "delta_index"};
	for (idx_t row = 0; row < target_input.size(); row++) {
		LerobotTemporalTarget target;
		target.request_id = ReadInputInteger(formats, bind_data.input_columns[0], row, input_names[0]);
		target.target_ordinal = static_cast<int64_t>(first_ordinal + row);
		target.target_id = bind_data.input_columns.size() == 5
		                       ? ReadInputInteger(formats, bind_data.input_columns[4], row, "target_id")
		                       : 0;
		target.episode_index = ReadInputInteger(formats, bind_data.input_columns[1], row, input_names[1]);
		target.frame_index = ReadInputInteger(formats, bind_data.input_columns[2], row, input_names[2]);
		target.delta_index = ReadInputInteger(formats, bind_data.input_columns[3], row, input_names[3]);

		if (target.episode_index < 0 || target.frame_index < 0) {
			throw InvalidInputException("lerobot_temporal_targets episode_index and frame_index must be non-negative");
		}
		if (target.delta_index < 0 || static_cast<uint64_t>(target.delta_index) >= bind_data.deltas.size()) {
			throw InvalidInputException("lerobot_temporal_targets delta_index %d is outside delta_timestamps length %d",
			                            target.delta_index, bind_data.deltas.size());
		}
		const auto route = bind_data.metadata->FindEpisodeRoute(target.episode_index);
		if (!route) {
			throw InvalidInputException("LeRobot episode %d does not exist", target.episode_index);
		}
		if (target.frame_index >= route->episode_length) {
			throw InvalidInputException("LeRobot frame %d is outside episode %d length %d", target.frame_index,
			                            target.episode_index, route->episode_length);
		}

		const auto &delta = bind_data.deltas[static_cast<idx_t>(target.delta_index)];
		target.delta_timestamp = delta.timestamp;
		target.delta_frame_offset = delta.frame_offset;
		const auto last_frame_index = route->episode_length - 1;
		target.is_padding = false;
		if (delta.frame_offset < -target.frame_index) {
			target.target_frame_index = 0;
			target.is_padding = true;
		} else if (delta.frame_offset > last_frame_index - target.frame_index) {
			target.target_frame_index = last_frame_index;
			target.is_padding = true;
		} else {
			target.target_frame_index = target.frame_index + delta.frame_offset;
		}

		for (idx_t output_column = 0; output_column < global_state.projected_columns.size(); output_column++) {
			WriteTargetColumn(target, global_state.projected_columns[output_column], row, output.data[output_column]);
		}
	}
	SetOutputCardinality(output, target_input.size());
	return OperatorResultType::NEED_MORE_INPUT;
}

} // namespace

TableFunctionSet LerobotFunctions::GetTemporalTargetsFunction() {
	TableFunction function("lerobot_temporal_targets", {LogicalType::VARCHAR, LogicalType::TABLE}, nullptr,
	                       LerobotTemporalTargetsBind, LerobotTemporalTargetsInitGlobal);
	function.in_out_function = LerobotTemporalTargetsFunction;
	function.named_parameters["delta_timestamps"] = LogicalType::LIST(LogicalType::DOUBLE);
	function.named_parameters["tolerance"] = LogicalType::DOUBLE;
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	function.projection_pushdown = true;
	return TableFunctionSet(std::move(function));
}

} // namespace duckdb
