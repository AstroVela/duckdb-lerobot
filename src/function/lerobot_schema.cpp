#include "function/lerobot_schema.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

LogicalType LerobotDtypeToLogicalType(const string &dtype) {
	if (dtype == "float32") {
		return LogicalType::FLOAT;
	}
	if (dtype == "float64") {
		return LogicalType::DOUBLE;
	}
	if (dtype == "int8") {
		return LogicalType::TINYINT;
	}
	if (dtype == "int16") {
		return LogicalType::SMALLINT;
	}
	if (dtype == "int32") {
		return LogicalType::INTEGER;
	}
	if (dtype == "int64") {
		return LogicalType::BIGINT;
	}
	if (dtype == "uint8") {
		return LogicalType::UTINYINT;
	}
	if (dtype == "uint16") {
		return LogicalType::USMALLINT;
	}
	if (dtype == "uint32") {
		return LogicalType::UINTEGER;
	}
	if (dtype == "uint64") {
		return LogicalType::UBIGINT;
	}
	if (dtype == "bool") {
		return LogicalType::BOOLEAN;
	}
	if (dtype == "string") {
		return LogicalType::VARCHAR;
	}
	throw BinderException("Unsupported LeRobot v3 feature dtype '%s'", dtype);
}

LogicalType LerobotFeatureStorageType(const string &dtype, const vector<idx_t> &shape) {
	if (dtype == "image" || dtype == "video") {
		return LogicalType::BLOB;
	}
	auto result = LerobotDtypeToLogicalType(dtype);
	if (shape.size() == 1 && shape[0] == 1) {
		return result;
	}
	for (auto entry = shape.rbegin(); entry != shape.rend(); ++entry) {
		result = LogicalType::ARRAY(result, optional_idx(*entry));
	}
	return result;
}

LogicalType LerobotFeatureOutputType(const string &dtype, const vector<idx_t> &shape) {
	if (dtype == "image") {
		return LogicalType::STRUCT({{"bytes", LogicalType::BLOB}, {"path", LogicalType::VARCHAR}});
	}
	return LerobotFeatureStorageType(dtype, shape);
}

LogicalType LerobotFeatureScanType(const string &dtype, const vector<idx_t> &shape) {
	if (dtype == "image") {
		return LogicalType::STRUCT({{"bytes", LogicalType::BLOB}, {"path", LogicalType::VARCHAR}});
	}
	auto result = LerobotDtypeToLogicalType(dtype);
	if (shape.size() == 1 && shape[0] == 1) {
		return result;
	}
	// DuckDB ARRAY values are represented by Parquet LIST nodes, which do not
	// retain fixed lengths. Reconstruct the type exposed by parquet_scan.
	for (idx_t dimension = 0; dimension < shape.size(); dimension++) {
		result = LogicalType::LIST(result);
	}
	return result;
}

bool LerobotHasIntegerExtrema(const string &dtype) {
	return dtype != "image" && dtype != "video" && dtype != "string" && LerobotDtypeToLogicalType(dtype).IsIntegral();
}

LogicalType LerobotStatExtremaLeafType(const string &dtype) {
	if (!LerobotHasIntegerExtrema(dtype)) {
		return LogicalType::DOUBLE;
	}
	// Native episode metadata converts NumPy arrays to Python lists before
	// PyArrow inference, which canonicalizes the supported integer values to
	// int64. uint64 needs its unsigned leaf to cover the complete input domain.
	return dtype == "uint64" ? LogicalType::UBIGINT : LogicalType::BIGINT;
}

LogicalType LerobotNestedListType(const LogicalType &leaf, idx_t dimensions) {
	auto result = leaf;
	for (idx_t index = 0; index < dimensions; index++) {
		result = LogicalType::LIST(result);
	}
	return result;
}

} // namespace duckdb
