#ifdef LEROBOT_VANE_DISTRIBUTED
#include "../function/copy/lerobot_copy.cpp"
#include "vane/lerobot_vane.hpp"
#include "lerobot_copy_serde.hpp"
#include "duckdb/execution/distributed/extension_write_task_provider.hpp"
#include "duckdb/execution/operator/persistent/physical_copy_to_file.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {
namespace {
static const char *VANE_COPY_ORDINAL = "__lerobot_vane_ordinal";

struct VaneStageBind {
	string directory;
	vector<string> names;
	vector<LogicalType> types;
	string Encode() const {
		MemoryStream stream;
		BinarySerializer s(stream);
		s.Begin();
		s.WriteProperty(1, "directory", directory);
		s.WriteProperty(2, "names", names);
		s.WriteProperty(3, "types", types);
		s.End();
		return string(reinterpret_cast<const char *>(stream.GetData()), stream.GetPosition());
	}
	static VaneStageBind Decode(const string &bytes) {
		MemoryStream stream(reinterpret_cast<data_ptr_t>(const_cast<char *>(bytes.data())), bytes.size());
		BinaryDeserializer d(stream);
		d.Begin();
		VaneStageBind result;
		result.directory = d.ReadProperty<string>(1, "directory");
		result.names = d.ReadProperty<vector<string>>(2, "names");
		result.types = d.ReadProperty<vector<LogicalType>>(3, "types");
		d.End();
		if (result.directory.empty() || result.names.size() != result.types.size() || result.names.empty() ||
		    result.names.back() != VANE_COPY_ORDINAL || result.types.back() != LogicalType::BIGINT ||
		    stream.GetPosition() != bytes.size()) {
			throw SerializationException("Invalid LeRobot distributed COPY bind");
		}
		return result;
	}
};

struct VaneStageState : DistributedWriteGlobalState {
	VaneStageState(ClientContext &context, const VaneStageBind &bind)
	    : function(GetParquetCopyFunction(context)),
	      writer(function, BindParquet(context, function, bind.names, bind.types)), types(bind.types) {
		path = FileSystem::GetFileSystem(context).JoinPath(
		    bind.directory, "part-" + UUID::ToString(UUID::GenerateRandomUUID()) + ".parquet");
		writer.Open(context, path);
	}
	CopyFunction function;
	DelegatedParquetWriter writer;
	vector<LogicalType> types;
	string path;
	mutex lock;
	idx_t rows = 0;
};
struct VaneStageLocal : DistributedWriteLocalState {};

unique_ptr<DistributedWriteGlobalState> VaneStageInitialize(ClientContext &context,
                                                            const DistributedExtensionWriteInfo &info,
                                                            const DistributedWriteTaskContext &) {
	return make_uniq<VaneStageState>(context, VaneStageBind::Decode(info.worker_bind_data));
}
unique_ptr<DistributedWriteLocalState> VaneStageInitializeLocal(ExecutionContext &,
                                                                const DistributedExtensionWriteInfo &,
                                                                const DistributedWriteTaskContext &,
                                                                DistributedWriteGlobalState &) {
	return make_uniq<VaneStageLocal>();
}
void VaneStageSink(ExecutionContext &context, const DistributedExtensionWriteInfo &,
                   const DistributedWriteTaskContext &, DistributedWriteGlobalState &state,
                   DistributedWriteLocalState &, DataChunk &input) {
	if (!input.size()) {
		return;
	}
	auto &g = state.Cast<VaneStageState>();
	lock_guard<mutex> guard(g.lock);
	auto collection = make_uniq<ColumnDataCollection>(context.client, g.types);
	ColumnDataAppendState append;
	collection->InitializeAppend(append);
	collection->Append(append, input);
	g.writer.Flush(context.client, std::move(collection));
	g.rows += input.size();
}
void VaneStageCombine(ExecutionContext &, const DistributedExtensionWriteInfo &, const DistributedWriteTaskContext &,
                      DistributedWriteGlobalState &, DistributedWriteLocalState &) {
}
vector<DistributedWriteFragment> VaneStageFinalize(ClientContext &context, const DistributedExtensionWriteInfo &,
                                                   const DistributedWriteTaskContext &,
                                                   DistributedWriteGlobalState &state) {
	auto &g = state.Cast<VaneStageState>();
	g.writer.Close(context);
	DistributedWriteArtifact artifact;
	artifact.artifact_id = UUID::ToString(UUID::GenerateRandomUUID());
	artifact.uri = g.path;
	artifact.codec = {"lerobot.input-parquet", 1};
	artifact.payload = "v1";
	DistributedWriteFragment fragment;
	fragment.fragment_id = artifact.artifact_id;
	fragment.payload = "v1";
	fragment.row_count = g.rows;
	fragment.artifacts.push_back(std::move(artifact));
	return {std::move(fragment)};
}

class PhysicalVaneCopy : public PhysicalCopyToFile, public distributed::ExtensionWriteTaskProvider {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;
	PhysicalVaneCopy(PhysicalPlan &plan, LogicalCopyToFile &copy, PhysicalOperator &child)
	    : PhysicalCopyToFile(plan, copy.types, copy.function, std::move(copy.bind_data), copy.estimated_cardinality) {
		type = TYPE;
		file_path = copy.file_path;
		use_tmp_file = false;
		filename_pattern = copy.filename_pattern;
		file_extension = copy.file_extension;
		overwrite_mode = copy.overwrite_mode;
		parallel = false;
		per_thread_output = false;
		rotate = false;
		return_type = CopyFunctionReturnType::CHANGED_ROWS;
		partition_output = false;
		write_partition_columns = false;
		write_empty_file = true;
		hive_file_pattern = true;
		names = copy.names;
		expected_types = copy.expected_types;
		children.push_back(child);
		stage.directory = file_path + ".vane-" + UUID::ToString(UUID::GenerateRandomUUID());
		stage.types = bind_data->Cast<LerobotCopyBindData>().input_types;
		for (idx_t i = 0; i < stage.types.size(); i++) {
			stage.names.push_back("c" + std::to_string(i));
		}
		stage.names.push_back(VANE_COPY_ORDINAL);
		stage.types.push_back(LogicalType::BIGINT);
		write_plan.extension_name = "lerobot";
		write_plan.operator_name = "copy";
		write_plan.worker_bind_data = stage.Encode();
	}
	string GetName() const override {
		return "LEROBOT_COPY";
	}
	optional_ptr<distributed::ExtensionWriteTaskProvider> GetExtensionWriteTaskProvider() override {
		return this;
	}
	const distributed::DistributedExtensionWritePlan &WritePlan() const override {
		return write_plan;
	}
	void ValidateDistributedWrite(ClientContext &context) const override {
		auto &fs = FileSystem::GetFileSystem(context);
		if (fs.IsRemoteFile(file_path)) {
			throw NotImplementedException("Distributed FORMAT lerobot requires a shared local filesystem");
		}
		if (fs.FileExists(file_path) || fs.DirectoryExists(file_path)) {
			throw IOException("LeRobot dataset root already exists: '%s'", file_path);
		}
		if (fs.FileExists(stage.directory) || fs.DirectoryExists(stage.directory)) {
			throw IOException("LeRobot COPY execution has already been prepared");
		}
	}
	void PrepareDistributedWrite(ClientContext &context) const override {
		ValidateDistributedWrite(context);
		prepared = true;
		FileSystem::GetFileSystem(context).CreateDirectoriesRecursive(stage.directory);
	}
	void Cleanup(ClientContext &context) const noexcept {
		if (!prepared) {
			return;
		}
		try {
			auto &fs = FileSystem::GetFileSystem(context);
			if (fs.DirectoryExists(stage.directory)) {
				fs.RemoveDirectory(stage.directory);
			}
		} catch (const std::exception &error) {
			try {
				DUCKDB_LOG_WARNING(context, "LeRobot COPY staging cleanup failed: %s", error.what());
			} catch (...) {
			}
		}
	}
	void AbortDistributedWrite(ClientContext &context, const vector<DistributedWriteTaskResult> &) const override {
		Cleanup(context);
	}
	idx_t FinalizeDistributedWrite(ClientContext &context,
	                               const vector<DistributedWriteTaskResult> &results) const override {
		try {
			vector<Value> paths;
			unordered_set<string> selected;
			idx_t expected_rows = 0;
			for (const auto &task : results) {
				for (const auto &fragment : task.fragments) {
					if (fragment.payload != "v1" || fragment.artifacts.size() != 1) {
						throw SerializationException("Invalid LeRobot COPY fragment");
					}
					const auto &artifact = fragment.artifacts[0];
					if (artifact.codec.name != "lerobot.input-parquet" || artifact.codec.version != 1 ||
					    artifact.payload != "v1" || StringUtil::GetFilePath(artifact.uri) != stage.directory ||
					    !StringUtil::StartsWith(StringUtil::GetFileName(artifact.uri), "part-") ||
					    !StringUtil::EndsWith(artifact.uri, ".parquet") || !selected.insert(artifact.uri).second) {
						throw SerializationException("Invalid or duplicate LeRobot COPY artifact");
					}
					if (fragment.row_count > NumericLimits<idx_t>::Maximum() - expected_rows) {
						throw SerializationException("LeRobot COPY fragment row count overflow");
					}
					paths.emplace_back(artifact.uri);
					expected_rows += fragment.row_count;
				}
			}
			const auto &bind = bind_data->Cast<LerobotCopyBindData>();
			LerobotCopyGlobalData writer(context, bind, file_path);
			idx_t rows = 0;
			if (!paths.empty()) {
				string sql = "SELECT ";
				for (idx_t i = 0; i < bind.input_types.size(); i++) {
					if (i) {
						sql += ", ";
					}
					sql += "CAST(c" + std::to_string(i) + " AS " + bind.input_types[i].ToString() + ")";
				}
				sql += ", " + string(VANE_COPY_ORDINAL) + " FROM read_parquet(" +
				       Value::LIST(LogicalType::VARCHAR, std::move(paths)).ToSQLString() + ") ORDER BY " +
				       VANE_COPY_ORDINAL;
				LerobotNestedQuery query(context, sql, true);
				while (auto chunk = query.Fetch()) {
					for (idx_t row = 0; row < chunk->size(); row++) {
						auto ordinal = chunk->GetValue(bind.input_types.size(), row);
						if (ordinal.IsNull() || ordinal.GetValue<int64_t>() != NumericCast<int64_t>(++rows)) {
							throw IOException("Distributed LeRobot COPY input order has missing or duplicate rows");
						}
					}
					writer.Process(*chunk);
				}
			}
			if (rows != expected_rows) {
				throw IOException("LeRobot COPY fragment row count mismatch");
			}
			writer.Finalize();
			Cleanup(context);
			return rows;
		} catch (...) {
			Cleanup(context);
			throw;
		}
	}

private:
	VaneStageBind stage;
	distributed::DistributedExtensionWritePlan write_plan;
	mutable bool prepared = false;
};

struct LogicalVaneCopy : LogicalExtensionOperator {
	explicit LogicalVaneCopy(unique_ptr<LogicalCopyToFile> copy_p) : copy(std::move(copy_p)) {
		children = std::move(copy->children);
		types = copy->types;
		estimated_cardinality = copy->estimated_cardinality;
	}
	unique_ptr<LogicalCopyToFile> copy;
	vector<ColumnBinding> GetColumnBindings() override {
		return copy->GetColumnBindings();
	}
	string GetExtensionName() const override {
		return "lerobot_vane_copy";
	}
	string GetName() const override {
		return "LEROBOT_COPY";
	}
	void ResolveTypes() override {
		types = {LogicalType::BIGINT};
	}
	void Serialize(Serializer &s) const override {
		LogicalExtensionOperator::Serialize(s);
		s.WriteProperty(201, "copy", copy);
	}
	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override {
		auto &fs = FileSystem::GetFileSystem(context);
		copy->file_path = fs.ExpandPath(copy->file_path);
		// Match the native writer before deriving the sibling staging directory.
		// Otherwise "dataset/" creates "dataset/.vane-*" and reserves the final root.
		const auto separator = fs.PathSeparator(copy->file_path);
		while (!copy->file_path.empty()) {
			if (copy->file_path.back() == '/') {
				copy->file_path.pop_back();
			} else if (!separator.empty() && StringUtil::EndsWith(copy->file_path, separator)) {
				copy->file_path.resize(copy->file_path.size() - separator.size());
			} else {
				break;
			}
		}
		if (copy->file_path.empty()) {
			throw IOException("LeRobot dataset root cannot be empty");
		}
		if (!fs.IsPathAbsolute(copy->file_path)) {
			copy->file_path = fs.JoinPath(fs.GetWorkingDirectory(), copy->file_path);
		}
		return planner.Make<PhysicalVaneCopy>(*copy, planner.CreatePlan(*children[0]));
	}
};
class VaneCopyOperatorExtension : public OperatorExtension {
public:
	VaneCopyOperatorExtension() {
		// DuckDB invokes every registered operator extension after a bind error.
		// This extension only deserializes our optimizer-created COPY operator.
		Bind = [](ClientContext &, Binder &, OperatorExtensionInfo *, SQLStatement &) {
			return BoundStatement();
		};
	}
	string GetName() override {
		return "lerobot_vane_copy";
	}
	unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &d) override {
		auto copy = d.ReadProperty<unique_ptr<LogicalOperator>>(201, "copy");
		if (copy->type != LogicalOperatorType::LOGICAL_COPY_TO_FILE) {
			throw SerializationException("Invalid LeRobot COPY operator");
		}
		return make_uniq<LogicalVaneCopy>(unique_ptr_cast<LogicalOperator, LogicalCopyToFile>(std::move(copy)));
	}
};
void VaneCopyOptimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	for (auto &child : plan->children) {
		VaneCopyOptimize(input, child);
	}
	// The native target ordinal uses a process-local counter. Generate its public
	// value with a global window so disjoint worker inputs cannot reuse ordinals.
	if (plan->type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = plan->Cast<LogicalGet>();
		if (get.function.name != "lerobot_temporal_targets" && get.function.name != "lerobot_video_targets") {
			return;
		}
		if (!get.projection_ids.empty() || !get.projected_input.empty()) {
			throw NotImplementedException("Vane LeRobot targets do not support projected input passthrough");
		}
		optional_idx ordinal_column;
		for (idx_t i = 0; i < get.GetColumnIds().size(); i++) {
			if (get.GetColumnIds()[i].GetPrimaryIndex() == 1) {
				ordinal_column = i;
			}
		}
		if (!ordinal_column.IsValid()) {
			return;
		}
		get.ResolveOperatorTypes();
		auto public_index = get.table_index;
		get.table_index = input.optimizer.binder.GenerateTableIndex();
		auto bindings = get.GetColumnBindings();
		auto window_index = input.optimizer.binder.GenerateTableIndex();
		auto window = make_uniq<LogicalWindow>(window_index);
		auto ordinal =
		    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
		ordinal->start = WindowBoundary::UNBOUNDED_PRECEDING;
		ordinal->end = WindowBoundary::CURRENT_ROW_ROWS;
		window->expressions.push_back(std::move(ordinal));
		vector<unique_ptr<Expression>> projection;
		for (idx_t i = 0; i < bindings.size(); i++) {
			if (i == ordinal_column.GetIndex()) {
				vector<unique_ptr<Expression>> arguments;
				arguments.push_back(
				    make_uniq<BoundColumnRefExpression>(LogicalType::BIGINT, ColumnBinding(window_index, 0)));
				arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(1)));
				ErrorData error;
				auto expression =
				    FunctionBinder(input.context).BindScalarFunction(DEFAULT_SCHEMA, "-", std::move(arguments), error);
				if (!expression) {
					error.Throw();
				}
				projection.push_back(std::move(expression));
			} else {
				projection.push_back(make_uniq<BoundColumnRefExpression>(get.types[i], bindings[i]));
			}
		}
		window->children.push_back(std::move(plan));
		auto result = make_uniq<LogicalProjection>(public_index, std::move(projection));
		result->children.push_back(std::move(window));
		result->ResolveOperatorTypes();
		plan = std::move(result);
		return;
	}
	if (plan->type != LogicalOperatorType::LOGICAL_COPY_TO_FILE) {
		return;
	}
	auto &copy = plan->Cast<LogicalCopyToFile>();
	if (copy.function.name != "lerobot") {
		return;
	}
	if (copy.partition_output || copy.per_thread_output || copy.rotate || copy.file_size_bytes.IsValid() ||
	    copy.use_tmp_file || copy.return_type != CopyFunctionReturnType::CHANGED_ROWS) {
		throw NotImplementedException("Vane FORMAT lerobot supports one new dataset with a row-count result");
	}
	// A global row number transports the user's input order across task attempts.
	// It is internal and is never included in the dataset's feature schema.
	auto window = make_uniq<LogicalWindow>(input.optimizer.binder.GenerateTableIndex());
	auto ordinal =
	    make_uniq<BoundWindowExpression>(ExpressionType::WINDOW_ROW_NUMBER, LogicalType::BIGINT, nullptr, nullptr);
	ordinal->start = WindowBoundary::UNBOUNDED_PRECEDING;
	ordinal->end = WindowBoundary::CURRENT_ROW_ROWS;
	window->expressions.push_back(std::move(ordinal));
	window->children.push_back(std::move(copy.children[0]));
	window->ResolveOperatorTypes();
	copy.children[0] = std::move(window);
	plan = make_uniq<LogicalVaneCopy>(unique_ptr_cast<LogicalOperator, LogicalCopyToFile>(std::move(plan)));
}
} // namespace

void RegisterLerobotVaneCopy(ExtensionLoader &loader) {
	auto copy = LerobotCopyFunction::Create();
	copy.serialize = VaneCopySerialize;
	copy.deserialize = VaneCopyDeserialize;
	loader.RegisterFunction(std::move(copy));
	DistributedWriteOperatorExtension extension;
	extension.name = "copy";
	extension.protocol_version = 1;
	extension.mode = DistributedWriteMode::CALLBACK_SINK;
	extension.fragment_codec = {"lerobot.input-fragment", 1};
	extension.callbacks = {VaneStageInitialize, VaneStageInitializeLocal, VaneStageSink, VaneStageCombine,
	                       VaneStageFinalize};
	DistributedWriteOperatorExtension::Register(loader, std::move(extension));
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	OptimizerExtension optimizer;
	optimizer.optimize_function = VaneCopyOptimize;
	OptimizerExtension::Register(config, std::move(optimizer));
	OperatorExtension::Register(config, make_shared_ptr<VaneCopyOperatorExtension>());
}
} // namespace duckdb
#endif
