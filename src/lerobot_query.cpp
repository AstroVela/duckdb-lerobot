#include "lerobot_query.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>

namespace duckdb {

namespace {

struct NestedCancellationEntry {
	NestedCancellationEntry(ClientContext &parent_p, ClientContext &child_p) : parent(parent_p), child(child_p) {
	}

	ClientContext &parent;
	ClientContext &child;
	atomic<bool> cancelled {false};
};

class NestedCancellationMonitor final : public ObjectCacheEntry {
public:
	~NestedCancellationMonitor() override {
		{
			lock_guard<mutex> guard(lock);
			D_ASSERT(entries.empty());
			stopping = true;
		}
		cv.notify_all();
		if (worker) {
			worker->join();
		}
	}

	static string ObjectType() {
		return "lerobot_nested_query_cancellation";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		// A single monitor per database, sleeping while no reads are registered.
		return optional_idx();
	}

	void Register(NestedCancellationEntry &entry) {
		lock_guard<mutex> guard(lock);
		entries.push_back(&entry);
		try {
			if (!worker) {
				worker = make_uniq<thread>([this] { Run(); });
			}
		} catch (...) {
			entries.pop_back();
			throw;
		}
		cv.notify_all();
	}

	void Unregister(NestedCancellationEntry &entry) {
		// The lock makes removal a lifetime barrier: no monitor access to either
		// borrowed context remains when the caller destroys its connection.
		lock_guard<mutex> guard(lock);
		auto found = std::find(entries.begin(), entries.end(), &entry);
		D_ASSERT(found != entries.end());
		entries.erase(found);
		cv.notify_all();
	}

private:
	void Run() {
		unique_lock<mutex> guard(lock);
		while (!stopping) {
			if (entries.empty()) {
				cv.wait(guard, [this] { return stopping || !entries.empty(); });
				continue;
			}
			for (auto entry : entries) {
				// Borrow under the registration lock. Taking owning context
				// references here could destroy the database/cache on this
				// thread when the last reference is released, causing self-join.
				if (entry->parent.IsInterrupted()) {
					entry->cancelled = true;
				}
				if (entry->cancelled.load()) {
					// Query startup clears the child's interrupt flag. Re-send
					// until unregistered so an early signal cannot be lost.
					entry->child.Interrupt();
				}
			}
			cv.wait_for(guard, std::chrono::milliseconds(10));
		}
	}

	mutex lock;
	std::condition_variable cv;
	vector<NestedCancellationEntry *> entries;
	unique_ptr<thread> worker;
	bool stopping = false;
};

struct NestedCancellationScope {
	NestedCancellationScope(ClientContext &parent, ClientContext &child) : entry(parent, child) {
		monitor = ObjectCache::GetObjectCache(parent).GetOrCreate<NestedCancellationMonitor>(
		    "__duckdb_lerobot_nested_query_cancellation");
		if (!monitor) {
			throw InternalException("LeRobot nested query monitor cache key has an incompatible object type");
		}
		monitor->Register(entry);
	}
	~NestedCancellationScope() {
		monitor->Unregister(entry);
	}
	void CheckInterrupted() const {
		if (entry.parent.IsInterrupted() || entry.cancelled.load()) {
			throw InterruptException();
		}
	}

	NestedCancellationEntry entry;
	shared_ptr<NestedCancellationMonitor> monitor;
};

} // namespace

void InheritLerobotReaderSettings(ClientContext &parent, ClientContext &reader) {
	D_ASSERT(parent.db == reader.db);
	auto &parent_settings = ClientConfig::GetConfig(parent).user_settings;
	auto &reader_settings = ClientConfig::GetConfig(reader).user_settings;
	for (const auto &entry : DBConfig::GetConfig(parent).GetExtensionSettings()) {
		const auto index = entry.second.setting_index.GetIndex();
		if (!parent_settings.IsSet(index)) {
			continue;
		}
		Value value;
		if (parent.TryGetCurrentUserSetting(index, value)) {
			// Values have already passed the caller's SET validation. Copy
			// them directly, without SQL interpolation or replaying callbacks
			// that could mutate global state from a pipeline worker.
			reader_settings.SetUserSetting(index, std::move(value));
		}
	}
}

void InitializeLerobotReaderTransaction(ClientContext &parent, Connection &reader) {
	if (parent.IsInterrupted()) {
		throw InterruptException();
	}
	if (parent.transaction.IsAutoCommit()) {
		return;
	}
	// BEGIN and the catalog lookup below pin the reader's system transaction.
	// Keep it open through all queries/fetches; Connection/TransactionContext
	// destruction rolls it back, including during constructor unwinding.
	reader.BeginTransaction();
	try {
		auto &manager = SecretManager::Get(parent);
		auto parent_secrets = manager.AllSecrets(CatalogTransaction::GetSystemCatalogTransaction(parent));
		vector<SecretEntry> reader_secrets;
		reader.context->RunFunctionInTransaction([&] {
			reader_secrets = manager.AllSecrets(CatalogTransaction::GetSystemCatalogTransaction(*reader.context));
		});
		auto validate_and_sort = [](const vector<SecretEntry> &entries) {
			vector<const KeyValueSecret *> secrets;
			for (const auto &entry : entries) {
				const auto *secret = entry.secret.get();
				// Only DuckDB's built-in temporary storage has the catalog MVCC
				// semantics required here. Do not serialize arbitrary secret classes:
				// custom serializers need not encode all lookup behavior.
				// macOS can give the same class different hidden type_info objects
				// across loadable extensions (notably HTTPFS). Compare exact ABI
				// type names rather than type_info identity; derived classes still
				// have distinct names and must not bypass this check.
				if (entry.storage_mode != SecretManager::TEMPORARY_STORAGE_NAME ||
				    entry.persist_type != SecretPersistType::TEMPORARY || !secret ||
				    std::strcmp(typeid(*secret).name(), typeid(KeyValueSecret).name()) != 0) {
					throw NotImplementedException(
					    "LeRobot explicit transactions support only built-in temporary "
					    "KeyValueSecret storage; persistent and third-party secrets are unsupported");
				}
				secrets.push_back(static_cast<const KeyValueSecret *>(secret));
			}
			std::sort(secrets.begin(), secrets.end(),
			          [](const KeyValueSecret *a, const KeyValueSecret *b) { return a->GetName() < b->GetName(); });
			return secrets;
		};
		auto parent_view = validate_and_sort(parent_secrets);
		auto reader_view = validate_and_sort(reader_secrets);
		bool equal = parent_view.size() == reader_view.size();
		for (idx_t i = 0; equal && i < parent_view.size(); i++) {
			const auto &a = *parent_view[i];
			const auto &b = *reader_view[i];
			equal = a.GetName() == b.GetName() && a.GetType() == b.GetType() && a.GetProvider() == b.GetProvider() &&
			        a.GetScope() == b.GetScope() && a.redact_keys == b.redact_keys &&
			        a.secret_map.size() == b.secret_map.size();
			auto left = a.secret_map.begin();
			auto right = b.secret_map.begin();
			for (; equal && left != a.secret_map.end(); ++left, ++right) {
				equal = left->first == right->first && left->second.type() == right->second.type() &&
				        Value::NotDistinctFrom(left->second, right->second);
			}
		}
		if (!equal) {
			throw InvalidInputException("LeRobot explicit transaction has different secret catalog views in the caller "
			                            "and internal reader; commit or roll back secret changes before reading");
		}
	} catch (const NotImplementedException &) {
		if (parent.IsInterrupted()) {
			throw InterruptException();
		}
		throw NotImplementedException("LeRobot explicit transactions support only built-in temporary KeyValueSecret "
		                              "storage; persistent and third-party secrets are unsupported");
	} catch (...) {
		if (parent.IsInterrupted()) {
			throw InterruptException();
		}
		// Backend and Value errors can contain credentials. Never forward them.
		throw InvalidInputException("LeRobot explicit transaction cannot verify equal secret catalog views in the "
		                            "caller and internal reader; commit or roll back secret changes before reading");
	}
	if (parent.IsInterrupted()) {
		throw InterruptException();
	}
}

struct LerobotNestedQuery::Impl {
	Impl(ClientContext &context, const string &sql, bool stream)
	    : connection(*context.db), cancellation(context, *connection.context) {
		cancellation.CheckInterrupted();
		InheritLerobotReaderSettings(context, *connection.context);
		try {
			InitializeLerobotReaderTransaction(context, connection);
			cancellation.CheckInterrupted();
			if (stream) {
				result = connection.SendQuery(sql);
			} else {
				result = connection.Query(sql);
			}
			CheckInterrupted();
		} catch (...) {
			cancellation.CheckInterrupted();
			throw;
		}
	}

	void CheckInterrupted() const {
		cancellation.CheckInterrupted();
		if (result->HasError() && result->GetErrorType() == ExceptionType::INTERRUPT) {
			throw InterruptException();
		}
	}

	// Destruction order is intentional, also when the constructor throws:
	// result cleanup -> unregister/wait for monitor -> destroy connection.
	Connection connection;
	NestedCancellationScope cancellation;
	unique_ptr<QueryResult> result;
};

LerobotNestedQuery::LerobotNestedQuery(ClientContext &context, const string &sql, bool stream) {
	if (context.IsInterrupted()) {
		throw InterruptException();
	}
	impl = make_uniq<Impl>(context, sql, stream);
}

LerobotNestedQuery::~LerobotNestedQuery() {
}

const QueryResult &LerobotNestedQuery::GetResult() const {
	impl->CheckInterrupted();
	return *impl->result;
}

unique_ptr<DataChunk> LerobotNestedQuery::Fetch() {
	impl->CheckInterrupted();
	try {
		auto chunk = impl->result->Fetch();
		impl->CheckInterrupted();
		return chunk;
	} catch (...) {
		impl->cancellation.CheckInterrupted();
		throw;
	}
}

} // namespace duckdb
