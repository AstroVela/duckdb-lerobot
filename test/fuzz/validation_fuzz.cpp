#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "function/lerobot_copy.hpp"
#include "function/lerobot_copy_options.hpp"
#include "function/lerobot_temporal.hpp"
#include "function/lerobot_video_options.hpp"
#include "lerobot_path.hpp"

#include <cmath>
#include <cstdlib>

using namespace duckdb;

namespace {

// Expected rejection is part of the public input contract. InternalException,
// bad_alloc and every other unexpected exception must reach libFuzzer.
template <class FUNC>
void AllowInputError(FUNC &&function) {
	try {
		function();
	} catch (const BinderException &) {
	} catch (const ConversionException &) {
	} catch (const InvalidInputException &) {
	} catch (const OutOfRangeException &) {
	}
}

void Check(bool condition) {
	if (!condition) {
		std::abort();
	}
}

struct FuzzDatabase {
	FuzzDatabase() {
		DBConfig config;
		config.options.load_extensions = false;
		config.options.maximum_threads = 1;
		config.SetOptionByName("enable_external_access", Value::BOOLEAN(false));
		db = make_uniq<DuckDB>(nullptr, &config);
		for (const auto &name : {"core_functions", "json", "parquet"}) {
			Check(ExtensionHelper::LoadExtension(*db, name) == ExtensionLoadResult::LOADED_EXTENSION);
		}
		connection = make_uniq<Connection>(*db);
	}
	unique_ptr<DuckDB> db;
	unique_ptr<Connection> connection;
};

FuzzDatabase &Database() {
	static FuzzDatabase database;
	return database;
}

Value Scalar(uint8_t kind, const string &text) {
	switch (kind % 6) {
	case 0:
		return Value();
	case 1:
		return Value(text).DefaultCastAs(LogicalType::BIGINT);
	case 2:
		return Value(text).DefaultCastAs(LogicalType::UBIGINT);
	case 3:
		return Value(text).DefaultCastAs(LogicalType::DOUBLE);
	case 4:
		return Value(text).DefaultCastAs(LogicalType::BOOLEAN);
	default:
		return Value(text);
	}
}

void Paths(const string &text) {
	AllowInputError([&] {
		auto root = NormalizeLerobotRoot(text);
		Check(!root.empty());
		Check(root.size() == 1 || root.back() != '/');
	});
	AllowInputError([&] {
		const string root = "/fuzz/dataset";
		const auto resolved = ResolveLerobotRelativePath(root, text, "fuzz path");
		Check(resolved == root + "/" + text);
		// Independent containment oracle for the portable path contract.
		Check(text.find('\\') == string::npos && text.find(':') == string::npos);
		idx_t offset = 0;
		while (offset <= text.size()) {
			const auto end = text.find('/', offset);
			const auto component = text.substr(offset, end == string::npos ? end : end - offset);
			Check(!component.empty() && component != "." && component != "..");
			if (end == string::npos) {
				break;
			}
			offset = end + 1;
		}
	});
	AllowInputError([&] {
		ValidateLerobotFeatureName(text);
		Check(ResolveLerobotRelativePath("/fuzz", text, "feature") == "/fuzz/" + text);
	});
}

void CopyOptions(uint8_t option, uint8_t kind, const string &text) {
	static const char *names[] = {"fps",
	                              "chunks_size",
	                              "metadata_buffer_size",
	                              "max_visual_frame_bytes",
	                              "video_workers",
	                              "encoder_threads",
	                              "data_files_size_in_mb",
	                              "video_files_size_in_mb",
	                              "rgb_codec",
	                              "rgb_crf",
	                              "rgb_gop",
	                              "depth_min",
	                              "depth_max",
	                              "depth_shift",
	                              "depth_use_log",
	                              "depth_clip",
	                              "robot_type"};
	CopyInfo info;
	info.options["fps"] = {Value::BIGINT(30)};
	info.options["features"] = {Value("{}")};
	info.options[names[option % (sizeof(names) / sizeof(names[0]))]] = {Scalar(kind, text)};
	CopyFunctionBindInput input(info);
	auto config = ParseLerobotCopyRequiredConfig(input);
	ParseLerobotCopyOptionalConfig(*Database().connection->context, input, config);
	Check(config.fps > 0 && config.chunks_size > 0 && config.metadata_buffer_size > 0);
	Check(config.encoder_threads == 1 && config.video_workers == 1);
}

void Features(uint8_t schema, const string &text) {
	CopyInfo info;
	info.options["fps"] = {Value::BIGINT(30)};
	info.options["features"] = {Value(text)};
	auto function = LerobotCopyFunction::Create();
	CopyFunctionBindInput input(info, function.function_info);
	vector<string> names = {"episode_index", "task"};
	vector<LogicalType> types = {LogicalType::BIGINT, LogicalType::VARCHAR};
	if (schema % 3 != 0) {
		names.push_back(schema % 3 == 1 ? "action" : "camera");
		types.push_back(schema % 3 == 1 ? LogicalType::FLOAT : LogicalType::BLOB);
	}
	// Call only binding: fuzz input never becomes SQL code, a filename or an
	// executed COPY. JSON still goes through the real nested native reader.
	auto &connection = *Database().connection;
	connection.BeginTransaction();
	try {
		auto bound = function.copy_to_bind(*connection.context, input, names, types);
		Check(bool(bound));
		connection.Rollback();
	} catch (...) {
		connection.Rollback();
		throw;
	}
}

void VideoOptions(uint8_t option, uint8_t kind, const string &text) {
	static const char *names[] = {"width",
	                              "height",
	                              "tolerance",
	                              "cluster_gap",
	                              "batch_size",
	                              "target_buffer_size",
	                              "max_cached_decoders",
	                              "decode_threads",
	                              "producer_threads",
	                              "max_pending_targets",
	                              "max_output_bytes",
	                              "codec_threads"};
	vector<Value> inputs;
	named_parameter_map_t parameters;
	vector<LogicalType> types;
	vector<string> columns;
	TableFunction function;
	TableFunctionRef ref;
	TableFunctionBindInput input(inputs, parameters, types, columns, nullptr, nullptr, function, ref);
	// A rejected option must not prevent an interesting timestamp being checked.
	AllowInputError([&] {
		const auto index = (option & 0x7f) % (sizeof(names) / sizeof(names[0]));
		const auto value = Scalar(kind, text);
		parameters[names[index]] = value;
		// Exercise both a missing peer dimension and a complete resize request.
		// A positive width alone is rejected before its upper bound is checked.
		if ((option & 0x80) && index < 2) {
			parameters[names[1 - index]] = value;
		}
		const auto result = GetLerobotVideoOptions(input, "fuzz");
		Check(result.width >= 0 && result.width <= 32768 && result.height >= 0 && result.height <= 32768);
		Check((result.width == 0) == (result.height == 0) && result.max_pending_targets > 0);
	});
	parameters.clear();
	parameters["delta_timestamps"] =
	    Value::LIST(LogicalType::DOUBLE, {kind == 0 ? Value(LogicalType::DOUBLE) : Scalar(3, text)});
	const int64_t fps = option == 255 ? NumericLimits<int64_t>::Maximum() : int64_t(option) + 1;
	const double tolerance = kind % 2 == 0 ? 1e-4 : 1.0;
	const auto deltas = GetLerobotTemporalDeltas(input, fps, tolerance, "fuzz");
	Check(deltas.size() == 1 && std::isfinite(deltas[0].timestamp));
	// IEEE remainder uses a ties-to-even quotient independently of the active
	// rounding mode. Check rounding itself, not only the accepted tolerance.
	const auto scaled = deltas[0].timestamp * double(fps);
	const auto rounded = scaled - std::remainder(scaled, 1.0);
	const auto int64_bound = std::ldexp(1.0, 63);
	Check(std::isfinite(rounded) && rounded >= -int64_bound && rounded < int64_bound);
	Check(deltas[0].frame_offset == static_cast<int64_t>(rounded));
	Check(std::fabs(deltas[0].timestamp - double(deltas[0].frame_offset) / double(fps)) <= tolerance);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size < 3 || size > 4096) {
		return 0;
	}
	const string text(reinterpret_cast<const char *>(data + 3), size - 3);
	AllowInputError([&] {
		switch (data[0] % 4) {
		case 0:
			Paths(text);
			break;
		case 1:
			CopyOptions(data[1], data[2], text);
			break;
		case 2:
			Features(data[1], text);
			break;
		default:
			VideoOptions(data[1], data[2], text);
			break;
		}
	});
	return 0;
}
