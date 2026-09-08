#pragma once

#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! A view of one scanned numeric column. The source vector must outlive the
//! view; Initialize must be called again after the next collection scan.
//! Only shape/selection/validity metadata is retained, not a converted chunk.
class LerobotNumericStatsVector {
public:
	void Initialize(Vector &input, idx_t count);
	void AppendRow(idx_t row, vector<double> &values, vector<Value> *exact_values) const;

private:
	struct Level {
		UnifiedVectorFormat data;
		idx_t array_size = 0;
	};
	using ReadValues = void (*)(const UnifiedVectorFormat &, idx_t, idx_t, vector<double> &, vector<Value> *);
	void AppendRange(idx_t level, idx_t offset, idx_t count, vector<double> &values, vector<Value> *exact_values) const;

	vector<Level> levels;
	ReadValues read_values = nullptr;
};

} // namespace duckdb
