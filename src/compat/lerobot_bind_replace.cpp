#include "compat/lerobot_bind_replace.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

#include <algorithm>

namespace duckdb {

namespace {

LogicalType BinaryAsStringType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BLOB:
		return LogicalType::VARCHAR;
	case LogicalTypeId::STRUCT: {
		child_list_t<LogicalType> children;
		for (const auto &child : StructType::GetChildTypes(type)) {
			children.emplace_back(child.first, BinaryAsStringType(child.second));
		}
		return LogicalType::STRUCT(std::move(children));
	}
	case LogicalTypeId::LIST:
		return LogicalType::LIST(BinaryAsStringType(ListType::GetChildType(type)));
	case LogicalTypeId::ARRAY:
		return LogicalType::ARRAY(BinaryAsStringType(ArrayType::GetChildType(type)), ArrayType::GetSize(type));
	case LogicalTypeId::MAP:
		return LogicalType::MAP(BinaryAsStringType(MapType::KeyType(type)),
		                        BinaryAsStringType(MapType::ValueType(type)));
	default:
		return type;
	}
}

bool GetBooleanOption(const named_parameter_map_t &parameters, const char *name) {
	auto entry = parameters.find(name);
	if (entry == parameters.end()) {
		return false;
	}
	if (entry->second.IsNull()) {
		throw BinderException("%s must not be NULL", name);
	}
	return BooleanValue::Get(entry->second);
}

void ValidateFilenameOption(const named_parameter_map_t &parameters) {
	auto filename = parameters.find("filename");
	if (filename == parameters.end()) {
		return;
	}
	if (filename->second.IsNull()) {
		throw BinderException("filename must not be NULL");
	}
	if (filename->second.type() != LogicalType::VARCHAR) {
		// MultiFileReader accepts ANY for filename. Reject values that cannot be
		// interpreted as its documented BOOLEAN alternative instead of silently
		// treating them as false.
		filename->second.DefaultCastAs(LogicalType::BOOLEAN);
	}
}

void AddEmptyParquetOptionColumns(ClientContext &context, const named_parameter_map_t &parameters,
                                  vector<string> &names, vector<LogicalType> &types) {
	// This option has no effect without files, but native read_parquet still
	// rejects NULL rather than silently accepting it.
	GetBooleanOption(parameters, "union_by_name");

	bool binary_as_string = false;
	auto binary_option = parameters.find("binary_as_string");
	if (binary_option != parameters.end()) {
		binary_as_string = GetBooleanOption(parameters, "binary_as_string");
	} else {
		Value setting;
		if (context.TryGetCurrentSetting("binary_as_string", setting)) {
			binary_as_string = BooleanValue::Get(setting);
		}
	}
	if (binary_as_string) {
		for (auto &type : types) {
			type = BinaryAsStringType(type);
		}
	}

	auto filename = parameters.find("filename");
	if (filename != parameters.end()) {
		bool enabled = false;
		string column_name = "filename";
		if (filename->second.type() == LogicalType::VARCHAR) {
			enabled = true;
			column_name = StringValue::Get(filename->second);
		} else {
			enabled = BooleanValue::Get(filename->second.DefaultCastAs(LogicalType::BOOLEAN));
		}
		if (enabled) {
			if (std::find(names.begin(), names.end(), column_name) != names.end()) {
				throw BinderException("filename column '%s' conflicts with the LeRobot schema", column_name);
			}
			names.push_back(std::move(column_name));
			types.push_back(LogicalType::VARCHAR);
		}
	}

	if (GetBooleanOption(parameters, "file_row_number")) {
		if (StringUtil::CIFind(names, "file_row_number") != DConstants::INVALID_INDEX) {
			throw BinderException("file_row_number conflicts with the LeRobot schema");
		}
		names.push_back("file_row_number");
		types.push_back(LogicalType::BIGINT);
	}
}

} // namespace

unique_ptr<TableFunctionRef> LerobotCreateTableFunctionRef(const char *name, Value argument,
                                                           const named_parameter_map_t &named_parameters) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ConstantExpression>(std::move(argument)));
	for (const auto &entry : named_parameters) {
		auto parameter = make_uniq<ConstantExpression>(entry.second);
		parameter->SetAlias(entry.first);
		children.push_back(std::move(parameter));
	}
	auto result = make_uniq<TableFunctionRef>();
	result->function = make_uniq<FunctionExpression>(name, std::move(children));
	return result;
}

Value LerobotCreatePathList(const vector<string> &paths) {
	vector<Value> values;
	values.reserve(paths.size());
	for (const auto &path : paths) {
		values.push_back(Value(path));
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(values));
}

void LerobotValidateParquetOptions(const named_parameter_map_t &parameters) {
	ValidateFilenameOption(parameters);
}

unique_ptr<TableRef> LerobotCreateEmptyParquetRelation(ClientContext &context, vector<string> names,
                                                       vector<LogicalType> types,
                                                       const named_parameter_map_t &parameters) {
	if (names.size() != types.size()) {
		throw InternalException("LeRobot empty Parquet schema names/types size mismatch");
	}
	LerobotValidateParquetOptions(parameters);
	AddEmptyParquetOptionColumns(context, parameters, names, types);
	auto select = make_uniq<SelectNode>();
	for (idx_t column = 0; column < names.size(); column++) {
		auto expression =
		    make_uniq<CastExpression>(types[column], make_uniq<ConstantExpression>(Value(LogicalType::SQLNULL)));
		expression->SetAlias(names[column]);
		select->select_list.push_back(std::move(expression));
	}
	// Parser-created SELECT statements without an explicit FROM receive this
	// node during transformation. Hand-built ASTs must add it themselves.
	select->from_table = make_uniq<EmptyTableRef>();
	select->where_clause = make_uniq<ConstantExpression>(Value::BOOLEAN(false));

	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(select);
	return make_uniq<SubqueryRef>(std::move(statement));
}

} // namespace duckdb
