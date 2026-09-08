#include "function/lerobot_numeric_stats.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

namespace {

template <class T>
void ReadNumericValues(const UnifiedVectorFormat &format, idx_t offset, idx_t count, vector<double> &values,
                       vector<Value> *exact_values) {
	const auto data = format.GetData<T>();
	for (idx_t i = 0; i < count; i++) {
		const auto index = format.sel->get_index(offset + i);
		if (!format.validity.RowIsValid(index)) {
			throw InvalidInputException("LeRobot feature values must not be NULL");
		}
		const auto value = data[index];
		// These are the schema's built-in numeric types. DuckDB's casts to
		// DOUBLE perform the same conversion; retain the DOUBLE intermediate
		// even for float32 statistics so reduction/rounding stays unchanged.
		values.push_back(static_cast<double>(value));
		if (exact_values) {
			// Only the first integer-extrema pass needs Values. Preserve the
			// original integer type and all 64 bits instead of round-tripping
			// through DOUBLE.
			exact_values->push_back(Value::CreateValue<T>(value));
		}
	}
}

} // namespace

void LerobotNumericStatsVector::Initialize(Vector &input, idx_t count) {
	levels.clear();
	auto current = &input;
	while (true) {
		levels.emplace_back();
		auto &level = levels.back();
		current->ToUnifiedFormat(count, level.data);
		if (current->GetType().id() != LogicalTypeId::ARRAY) {
			break;
		}
		level.array_size = ArrayType::GetSize(current->GetType());
		// A dictionary can reference rows beyond the selected row count;
		// constants can reference a single array. Use the actual child size.
		count = ArrayVector::GetTotalSize(*current);
		current = &ArrayVector::GetEntry(*current);
	}
	switch (current->GetType().id()) {
	case LogicalTypeId::BOOLEAN:
		read_values = ReadNumericValues<bool>;
		break;
	case LogicalTypeId::TINYINT:
		read_values = ReadNumericValues<int8_t>;
		break;
	case LogicalTypeId::SMALLINT:
		read_values = ReadNumericValues<int16_t>;
		break;
	case LogicalTypeId::INTEGER:
		read_values = ReadNumericValues<int32_t>;
		break;
	case LogicalTypeId::BIGINT:
		read_values = ReadNumericValues<int64_t>;
		break;
	case LogicalTypeId::UTINYINT:
		read_values = ReadNumericValues<uint8_t>;
		break;
	case LogicalTypeId::USMALLINT:
		read_values = ReadNumericValues<uint16_t>;
		break;
	case LogicalTypeId::UINTEGER:
		read_values = ReadNumericValues<uint32_t>;
		break;
	case LogicalTypeId::UBIGINT:
		read_values = ReadNumericValues<uint64_t>;
		break;
	case LogicalTypeId::FLOAT:
		read_values = ReadNumericValues<float>;
		break;
	case LogicalTypeId::DOUBLE:
		read_values = ReadNumericValues<double>;
		break;
	default:
		throw InvalidInputException("Unsupported LeRobot numeric statistics type '%s'", current->GetType());
	}
}

void LerobotNumericStatsVector::AppendRange(idx_t level_index, idx_t offset, idx_t count, vector<double> &values,
                                            vector<Value> *exact_values) const {
	const auto &level = levels[level_index];
	if (level.array_size == 0) {
		read_values(level.data, offset, count, values, exact_values);
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		const auto index = level.data.sel->get_index(offset + i);
		if (!level.data.validity.RowIsValid(index)) {
			throw InvalidInputException("LeRobot feature values must not be NULL");
		}
		AppendRange(level_index + 1, index * level.array_size, level.array_size, values, exact_values);
	}
}

void LerobotNumericStatsVector::AppendRow(idx_t row, vector<double> &values, vector<Value> *exact_values) const {
	D_ASSERT(read_values && !levels.empty());
	AppendRange(0, row, 1, values, exact_values);
}

} // namespace duckdb
