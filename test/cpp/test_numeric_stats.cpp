#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "function/lerobot_numeric_stats.hpp"
#include "function/lerobot_schema.hpp"

#include <cstring>

using namespace duckdb;

namespace {

void CheckRow(LerobotNumericStatsVector &reader, idx_t row, const vector<Value> &expected, bool exact) {
	vector<double> values;
	vector<Value> exact_values;
	reader.AppendRow(row, values, exact ? &exact_values : nullptr);
	REQUIRE(values.size() == expected.size());
	REQUIRE(exact_values.size() == (exact ? expected.size() : 0));
	for (idx_t i = 0; i < expected.size(); i++) {
		// Native casts are an independent reference for the conversion contract,
		// including signed zero and integer values beyond DOUBLE's precision.
		const auto reference = expected[i].DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
		REQUIRE(std::memcmp(&values[i], &reference, sizeof(double)) == 0);
		if (exact) {
			REQUIRE(exact_values[i].type() == expected[i].type());
			REQUIRE(exact_values[i] == expected[i]);
		}
	}
}

Value NestedRow(idx_t row) {
	vector<Value> children;
	for (idx_t outer = 0; outer < 2; outer++) {
		vector<Value> leaves;
		for (idx_t inner = 0; inner < 3; inner++) {
			leaves.push_back(Value::BIGINT(9007199254740993LL + row * 6 + outer * 3 + inner));
		}
		children.push_back(Value::ARRAY(LogicalType::BIGINT, leaves));
	}
	return Value::ARRAY(children[0].type(), children);
}

vector<Value> NestedExpected(idx_t row) {
	vector<Value> result;
	for (idx_t i = 0; i < 6; i++) {
		result.push_back(Value::BIGINT(9007199254740993LL + row * 6 + i));
	}
	return result;
}

} // namespace

TEST_CASE("Numeric statistics preserve native scalar casts and exact extrema", "[numeric_stats]") {
	const auto dtype = GENERATE("bool", "float32", "float64", "int8", "int16", "int32", "int64", "uint8", "uint16",
	                            "uint32", "uint64");
	const auto exact = GENERATE(false, true);
	CAPTURE(dtype, exact);
	const auto type = LerobotDtypeToLogicalType(dtype);
	vector<Value> expected {Value::MinimumValue(type), Value::MaximumValue(type),
	                        Value::INTEGER(0).DefaultCastAs(type)};
	if (type.id() == LogicalTypeId::FLOAT) {
		expected.push_back(Value::FLOAT(-0.0f));
	} else if (type.id() == LogicalTypeId::DOUBLE) {
		expected.push_back(Value::DOUBLE(-0.0));
	}
	Vector input(type);
	for (idx_t i = 0; i < expected.size(); i++) {
		input.SetValue(i, expected[i]);
	}
	LerobotNumericStatsVector reader;
	reader.Initialize(input, expected.size());
	for (idx_t i = 0; i < expected.size(); i++) {
		CheckRow(reader, i, {expected[i]}, exact);
	}
	// Reuse the same view for a dictionary and then a constant chunk.
	SelectionVector selection(3);
	selection.set_index(0, 1);
	selection.set_index(1, 0);
	selection.set_index(2, 1);
	Vector dictionary(input, selection, 3);
	reader.Initialize(dictionary, 3);
	for (idx_t i = 0; i < 3; i++) {
		CheckRow(reader, i, {expected[selection.get_index(i)]}, exact);
	}
	Vector constant(expected[1]);
	reader.Initialize(constant, 5);
	CheckRow(reader, 4, {expected[1]}, exact);
}

TEST_CASE("Numeric statistics map nested array selections at every level", "[numeric_stats]") {
	const auto dictionary_level = GENERATE(0, 1, 2);
	const auto exact = GENERATE(false, true);
	CAPTURE(dictionary_level, exact);
	// A child dictionary must cover the array's declared child cardinality,
	// including storage that native Vector::Flatten may normalize.
	Vector input(NestedRow(0).type(), 32);
	for (idx_t row = 0; row < 32; row++) {
		input.SetValue(row, NestedRow(row));
	}
	LerobotNumericStatsVector reader;
	reader.Initialize(input, 32);
	CheckRow(reader, 31, NestedExpected(31), exact);

	// A small parent selection can still reference the last row of a much
	// larger child buffer. Slice the outer arrays, inner arrays, or leaves.
	auto selected = &input;
	idx_t count = 32;
	for (idx_t depth = 0; depth < static_cast<idx_t>(dictionary_level); depth++) {
		count *= ArrayType::GetSize(selected->GetType());
		selected = &ArrayVector::GetEntry(*selected);
	}
	SelectionVector reverse(count);
	for (idx_t i = 0; i < count; i++) {
		reverse.set_index(i, count - i - 1);
	}
	selected->Slice(reverse, count);
	reader.Initialize(input, 2);
	vector<Value> expected;
	for (idx_t i = 0; i < 6; i++) {
		idx_t original = dictionary_level == 0   ? 31 * 6 + i
		                 : dictionary_level == 1 ? (63 - i / 3) * 3 + i % 3
		                                         : 191 - i;
		expected.push_back(Value::BIGINT(9007199254740993LL + original));
	}
	CheckRow(reader, 0, expected, exact);

	Vector constant(NestedRow(7));
	reader.Initialize(constant, 16);
	CheckRow(reader, 15, NestedExpected(7), exact);
}

TEST_CASE("Numeric statistics reject NULL at each array level and reset the view", "[numeric_stats]") {
	const auto null_level = GENERATE(0, 1, 2);
	Vector input(NestedRow(0).type());
	input.SetValue(0, NestedRow(0));
	input.SetValue(1, NestedRow(1));
	auto invalid = &input;
	for (idx_t depth = 0; depth < static_cast<idx_t>(null_level); depth++) {
		invalid = &ArrayVector::GetEntry(*invalid);
	}
	FlatVector::Validity(*invalid).SetInvalid(0);
	LerobotNumericStatsVector reader;
	reader.Initialize(input, 2);
	vector<double> values;
	REQUIRE_THROWS_AS(reader.AppendRow(0, values, nullptr), InvalidInputException);
	CheckRow(reader, 1, NestedExpected(1), false);
	Vector next(NestedRow(3));
	reader.Initialize(next, 1);
	CheckRow(reader, 0, NestedExpected(3), true);
}
