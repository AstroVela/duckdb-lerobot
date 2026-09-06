#include "catch.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/planner/extension_callback.hpp"
#include "function/lerobot_copy.hpp"

#include <condition_variable>
#include <future>

using namespace duckdb;

namespace {

enum class FeaturesStage { BEGIN, EXECUTE, END };

// Pause the real JSON query through DuckDB's context callbacks. The gate only
// observes the nested context; it cannot forward the outer interrupt itself.
struct FeaturesGate {
	void Pause(ClientContext &context, FeaturesStage current, optional_ptr<ErrorData> error = nullptr) {
		unique_lock<mutex> guard(lock);
		if (released || current != stage) {
			return;
		}
		if (!entered) {
			child = context.shared_from_this();
			entered = true;
			if (error && error->HasError()) {
				child_error = error->Type();
			}
		}
		cv.notify_all();
		while (!released) {
			if (context.IsInterrupted()) {
				cancelled = true;
				cv.notify_all();
			}
			cv.wait_for(guard, std::chrono::milliseconds(1));
		}
	}
	bool Wait(bool FeaturesGate::*flag) {
		unique_lock<mutex> guard(lock);
		return cv.wait_for(guard, std::chrono::seconds(5), [&] { return this->*flag; });
	}
	void Release() {
		lock_guard<mutex> guard(lock);
		released = true;
		cv.notify_all();
	}

	mutex lock;
	std::condition_variable cv;
	FeaturesStage stage = FeaturesStage::BEGIN;
	bool entered = false;
	bool cancelled = false;
	bool released = false;
	ExceptionType child_error = ExceptionType::INVALID;
	weak_ptr<ClientContext> child;
};

struct FeaturesQueryState final : public ClientContextState {
	explicit FeaturesQueryState(shared_ptr<FeaturesGate> gate_p) : gate(std::move(gate_p)) {
	}
	void QueryBegin(ClientContext &context) override {
		is_features = StringUtil::Contains(context.GetCurrentQuery(), "FROM json_each(json(");
		if (is_features) {
			gate->Pause(context, FeaturesStage::BEGIN);
		}
	}
	void OnTaskStart(ClientContext &context) override {
		if (is_features) {
			gate->Pause(context, FeaturesStage::EXECUTE);
		}
	}
	void QueryEnd(ClientContext &context, optional_ptr<ErrorData> error) override {
		if (is_features) {
			gate->Pause(context, FeaturesStage::END, error);
		}
	}
	shared_ptr<FeaturesGate> gate;
	bool is_features = false;
};

struct FeaturesConnections final : public ExtensionCallback {
	explicit FeaturesConnections(shared_ptr<FeaturesGate> gate_p) : gate(std::move(gate_p)) {
	}
	void OnConnectionOpened(ClientContext &context) override {
		context.registered_state->Insert("copy_features_test", make_shared_ptr<FeaturesQueryState>(gate));
	}
	shared_ptr<FeaturesGate> gate;
};

struct CopyBindTest {
	explicit CopyBindTest(idx_t threads = 1) {
		DBConfig config;
		config.options.load_extensions = false;
		config.options.maximum_threads = threads;
		db = make_uniq<DuckDB>(nullptr, &config);
		for (const auto &extension : {"core_functions", "json", "parquet"}) {
			REQUIRE(ExtensionHelper::LoadExtension(*db, extension) == ExtensionLoadResult::LOADED_EXTENSION);
		}
		connection = make_uniq<Connection>(*db);
		connection->BeginTransaction();
		CreateCopyFunctionInfo info(LerobotCopyFunction::Create());
		Catalog::GetSystemCatalog(*connection->context).CreateCopyFunction(*connection->context, info);
		connection->Commit();
		fs = FileSystem::CreateLocal();
		path = fs->JoinPath(FileSystem::GetWorkingDirectory(),
		                    "build/copy_bind_" + UUID::ToString(UUID::GenerateRandomUUID()));
		fs->CreateDirectoriesRecursive(path);
		gate = make_shared_ptr<FeaturesGate>();
		// The caller already exists. Only connections subsequently opened by
		// production COPY (or the independent-query check) receive this state.
		ExtensionCallbackManager::Get(*db->instance).Register(make_shared_ptr<FeaturesConnections>(gate));
	}
	~CopyBindTest() {
		// Even a failed assertion must release the gate before joining the COPY.
		connection->Interrupt();
		gate->Release();
		if (future.valid()) {
			future.wait();
		}
		fs->RemoveDirectory(path);
	}
	string SQL(const string &features = R"({"action":{"dtype":"float32","shape":[1]}})") const {
		return "COPY (SELECT 0::BIGINT AS episode_index, 'bind test' AS task, 7::FLOAT AS action) TO " +
		       Value(path + "/dataset").ToSQLString() + " (FORMAT lerobot, FPS 30, FEATURES " +
		       Value(features).ToSQLString() + ")";
	}
	void Start(const string &sql, bool prepare) {
		future = std::async(std::launch::async, [this, sql, prepare] {
			if (prepare) {
				auto result = connection->Prepare(sql);
				return result->HasError() ? result->GetErrorObject() : ErrorData();
			}
			auto result = connection->Query(sql);
			return result->HasError() ? result->GetErrorObject() : ErrorData();
		});
	}
	idx_t ConnectionCount() {
		return ConnectionManager::Get(*db->instance).GetConnectionCount();
	}
	void CheckClean() {
		REQUIRE(ConnectionCount() == 1);
		REQUIRE(gate->child.expired());
		vector<string> entries;
		fs->ListFiles(path, [&](const string &name, bool) { entries.push_back(name); });
		REQUIRE(entries.empty());
		auto result = connection->Query("SELECT 42");
		REQUIRE_FALSE(result->HasError());
		REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 42);
	}
	void CheckRetry() {
		gate->Release();
		auto prepared = connection->Prepare(SQL());
		INFO((prepared->HasError() ? prepared->GetError() : ""));
		REQUIRE_FALSE(prepared->HasError());
		auto result = prepared->Execute();
		INFO((result->HasError() ? result->GetError() : ""));
		REQUIRE_FALSE(result->HasError());
		auto rows = connection->Query("SELECT action FROM read_parquet(" +
		                              Value(path + "/dataset/data/chunk-000/file-000.parquet").ToSQLString() + ")");
		REQUIRE_FALSE(rows->HasError());
		REQUIRE(rows->RowCount() == 1);
		REQUIRE(rows->GetValue(0, 0).GetValue<float>() == 7);
		REQUIRE(ConnectionCount() == 1);
	}

	unique_ptr<DuckDB> db;
	unique_ptr<Connection> connection;
	unique_ptr<FileSystem> fs;
	string path;
	shared_ptr<FeaturesGate> gate;
	std::future<ErrorData> future;
};

void CancelFeatures(CopyBindTest &test) {
	REQUIRE(test.gate->Wait(&FeaturesGate::entered));
	REQUIRE(test.gate->child.lock() != test.connection->context);
	REQUIRE(test.ConnectionCount() == 2);
	test.connection->Interrupt();
	REQUIRE(test.gate->Wait(&FeaturesGate::cancelled));
	// Cancellation must wait for the nested query's own cleanup, not detach it.
	REQUIRE(test.future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
	{
		Connection other(*test.db);
		auto result = other.Query("SELECT 42");
		REQUIRE_FALSE(result->HasError());
		REQUIRE(result->GetValue(0, 0).GetValue<int32_t>() == 42);
	}
	test.gate->Release();
	REQUIRE(test.future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	auto error = test.future.get();
	INFO(error.Message());
	REQUIRE(error.HasError());
	REQUIRE(error.Type() == ExceptionType::INTERRUPT);
	REQUIRE_FALSE(StringUtil::Contains(error.Message(), "Failed to bind LeRobot FEATURES JSON"));
	test.CheckClean();
}

} // namespace

TEST_CASE("COPY FEATURES forwards cancellation during binding", "[copy_bind][copy_bind_cancel]") {
	const auto threads = GENERATE(1, 4);
	const auto prepare = GENERATE(false, true);
	const auto stage = GENERATE(FeaturesStage::BEGIN, FeaturesStage::EXECUTE, FeaturesStage::END);
	CAPTURE(threads, prepare, static_cast<int>(stage));
	CopyBindTest test(threads);
	test.gate->stage = stage;
	test.Start(test.SQL(), prepare);
	CancelFeatures(test);
	test.CheckRetry();
}

TEST_CASE("COPY FEATURES cancellation takes precedence over a nested JSON error", "[copy_bind]") {
	const auto prepare = GENERATE(false, true);
	CopyBindTest test;
	test.gate->stage = FeaturesStage::END;
	test.Start(test.SQL("{"), prepare);
	CancelFeatures(test);
	REQUIRE(test.gate->child_error == ExceptionType::INVALID_INPUT);
	test.CheckRetry();
}

TEST_CASE("COPY FEATURES validation errors preserve diagnostics and release connections", "[copy_bind]") {
	const auto features = GENERATE(string("{"), string(R"({"action":{"dtype":"float32","shape":["bad"]}})"),
	                               string(R"({"action":{"dtype":"float32","shape":[null]}})"),
	                               string(R"({"action":{"dtype":"float32","shape":[0]}})"));
	CopyBindTest test;
	test.gate->Release();
	for (idx_t i = 0; i < 3; i++) {
		auto result = test.connection->Prepare(test.SQL(features));
		REQUIRE(result->HasError());
		REQUIRE(result->GetErrorObject().Type() == ExceptionType::BINDER);
		if (features == "{" || StringUtil::Contains(features, "bad")) {
			REQUIRE(StringUtil::Contains(result->GetError(), "Failed to bind LeRobot FEATURES JSON"));
		} else {
			REQUIRE(StringUtil::Contains(result->GetError(), "LeRobot feature 'action' shape"));
		}
		test.CheckClean();
	}
	test.CheckRetry();
}
