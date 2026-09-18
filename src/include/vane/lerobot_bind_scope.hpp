#pragma once
#ifdef LEROBOT_VANE_DISTRIBUTED
#include "duckdb/function/table_function.hpp"
namespace duckdb {
template <class INPUT>
struct LerobotVaneBindScope {
	LerobotVaneBindScope(INPUT &input_p, const FunctionData &data) : input(input_p), previous(input.bind_data) {
		input.bind_data = &data;
	}
	~LerobotVaneBindScope() {
		input.bind_data = previous;
	}
	INPUT &input;
	optional_ptr<const FunctionData> previous;
};
} // namespace duckdb
#endif
