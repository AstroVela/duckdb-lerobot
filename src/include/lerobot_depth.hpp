//===----------------------------------------------------------------------===//
//                         DuckDB
//
// lerobot_depth.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cmath>

namespace duckdb {

// Validate the metre-space dequantization contract shared by readers and COPY.
// Encoding additionally validates float32 arithmetic in each accepted input unit.
inline bool LerobotValidFloat32DepthParameters(double depth_min, double depth_max, double shift, bool use_log) {
	const auto depth_min_float = static_cast<float>(depth_min);
	const auto depth_max_float = static_cast<float>(depth_max);
	const auto shift_float = static_cast<float>(shift);
	if (!std::isfinite(depth_min_float) || !std::isfinite(depth_max_float) || !std::isfinite(shift_float) ||
	    depth_min_float >= depth_max_float) {
		return false;
	}

	double scale;
	double offset;
	if (use_log) {
		const auto shifted_min = depth_min + shift;
		const auto shifted_max = depth_max + shift;
		if (!std::isfinite(shifted_min) || !std::isfinite(shifted_max) || shifted_min <= 0 ||
		    shifted_max <= shifted_min) {
			return false;
		}
		const auto log_min = std::log(shifted_min);
		const auto log_max = std::log(shifted_max);
		scale = (log_max - log_min) / 4095.0;
		offset = log_min;
	} else {
		scale = (depth_max - depth_min) / 4095.0;
		offset = depth_min;
	}
	const auto scale_float = static_cast<float>(scale);
	const auto offset_float = static_cast<float>(offset);
	return std::isfinite(scale) && std::isfinite(offset) && std::isfinite(scale_float) && scale_float > 0 &&
	       std::isfinite(offset_float);
}

} // namespace duckdb
