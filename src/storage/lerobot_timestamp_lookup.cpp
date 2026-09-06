#include "storage/lerobot_timestamp_lookup.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "lerobot_query.hpp"
#include "storage/lerobot_metadata_cache.hpp"

#include <algorithm>
#include <atomic>
#include <limits>

namespace duckdb {
namespace {

constexpr idx_t MAX_INDEX_BYTES = 32 * 1024 * 1024;
constexpr idx_t MAX_TRACKED_SHARDS = 64;
constexpr idx_t INDEX_ON_VISIT = 3;
constexpr idx_t MIN_INDEX_REQUESTS = 4096;
using Fingerprint = LerobotDatasetMetadata::FileFingerprint;

void CheckInterrupted(ClientContext &context) {
	if (context.IsInterrupted()) {
		throw InterruptException();
	}
}

Fingerprint ReadFingerprint(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	return Fingerprint(fs.GetFileSize(*handle), fs.GetLastModifiedTime(*handle), fs.GetVersionTag(*handle));
}

void CheckResult(const QueryResult &result, bool loading_index = false) {
	if (result.HasError()) {
		if (loading_index) {
			result.ThrowError();
		}
		// LerobotNestedQuery has already preserved cancellation. Retain the
		// existing timestamp lookup error contract for native reader errors.
		throw InvalidInputException("Failed to read LeRobot target timestamps: %s", result.GetError());
	}
}

struct IndexRow {
	int64_t episode;
	int64_t frame;
	double timestamp;
	bool has_timestamp;

	LerobotTimestampKey Key() const {
		return {episode, frame};
	}
};

struct IndexBudget {
	explicit IndexBudget(idx_t limit_p) : limit(limit_p) {
	}
	const idx_t limit;
	std::atomic<idx_t> bytes {0};
	std::atomic<idx_t> peak {0};
};

struct TimestampIndex {
	explicit TimestampIndex(shared_ptr<IndexBudget> budget_p) : budget(std::move(budget_p)) {
	}
	~TimestampIndex() {
		storage.Reset();
		budget->bytes.fetch_sub(bytes);
	}

	LerobotTimestampMatch Find(const LerobotTimestampKey &key) const {
		LerobotTimestampMatch result;
		if (count == 0) {
			return result;
		}
		const auto rows = reinterpret_cast<const IndexRow *>(storage.get());
		const auto begin =
		    std::lower_bound(rows, rows + count, key,
		                     [](const IndexRow &row, const LerobotTimestampKey &value) { return row.Key() < value; });
		const auto end =
		    std::upper_bound(begin, rows + count, key,
		                     [](const LerobotTimestampKey &value, const IndexRow &row) { return value < row.Key(); });
		result.count = end - begin;
		if (result.count == 1) {
			result.timestamp = begin->timestamp;
			result.has_timestamp = begin->has_timestamp;
		}
		return result;
	}

	shared_ptr<IndexBudget> budget;
	idx_t bytes = 0;
	idx_t count = 0;
	AllocatedData storage;
};

struct ShardSlot {
	unique_ptr<Fingerprint> fingerprint;
	idx_t visits = 0;
	idx_t last_use = 0;
	bool loading = false;
	bool skipped = false;
	shared_ptr<TimestampIndex> index;
};

void Merge(LerobotTimestampMatch &result, const LerobotTimestampMatch &part) {
	if (part.count > std::numeric_limits<int64_t>::max() - result.count) {
		throw InvalidInputException("Too many matching LeRobot Parquet rows");
	}
	result.count += part.count;
	if (part.has_timestamp) {
		result.timestamp = part.timestamp;
		result.has_timestamp = true;
	}
}

string FilteredQuery(const vector<string> &files, const vector<LerobotTimestampKey> &keys) {
	string values;
	for (const auto &key : keys) {
		if (!values.empty()) {
			values += ",";
		}
		values += "(" + std::to_string(key.first) + "," + std::to_string(key.second) + ")";
	}
	string paths = "[";
	for (idx_t i = 0; i < files.size(); i++) {
		paths += (i ? "," : "") + Value(files[i]).ToSQLString();
	}
	return "WITH requested(episode_index, frame_index) AS (VALUES " + values +
	       "), frames AS (SELECT CAST(episode_index AS BIGINT) AS episode_index, "
	       "CAST(frame_index AS BIGINT) AS frame_index, CAST(timestamp AS DOUBLE) AS timestamp "
	       "FROM read_parquet(" +
	       paths +
	       "])) SELECT CAST(requested.episode_index AS BIGINT), CAST(requested.frame_index AS BIGINT), "
	       "min(frames.timestamp), "
	       "CAST(count(frames.frame_index) AS BIGINT) FROM requested LEFT JOIN frames "
	       "USING (episode_index, frame_index) GROUP BY requested.episode_index, requested.frame_index";
}

} // namespace

struct LerobotTimestampLookup::Impl {
	Impl(ClientContext &context, idx_t max_bytes, idx_t min_requests_p)
	    : allocator(BufferManager::GetBufferManager(context).GetBufferAllocator()),
	      budget(make_shared_ptr<IndexBudget>(MinValue(max_bytes, MAX_INDEX_BYTES))), min_requests(min_requests_p) {
	}

	bool Reserve(TimestampIndex &index, idx_t bytes) {
		lock_guard<mutex> guard(lock);
		// Live readers and loaders retain their reservations even if their slot
		// is evicted. Eviction cannot make pinned memory disappear from the cap.
		while (bytes > budget->limit - budget->bytes.load()) {
			auto oldest = slots.end();
			for (auto it = slots.begin(); it != slots.end(); ++it) {
				if (it->second->index && (oldest == slots.end() || it->second->last_use < oldest->second->last_use)) {
					oldest = it;
				}
			}
			if (oldest == slots.end()) {
				return false;
			}
			oldest->second->index.reset();
			// Do not repeatedly rebuild full shards when a batch's working
			// set exceeds the cap. Retain native lookups for an evicted slot.
			oldest->second->skipped = true;
			evictions++;
		}
		index.bytes = bytes;
		const auto used = budget->bytes.fetch_add(bytes) + bytes;
		budget->peak = MaxValue(budget->peak.load(), used);
		return true;
	}

	shared_ptr<TimestampIndex> Load(ClientContext &context, const string &path) {
		const auto source = "read_parquet(" + Value(path).ToSQLString() + ")";
		idx_t row_count;
		{
			// Native count(*) uses the Parquet footer. Size the one allocation
			// before reading columns, rather than growing an unbounded vector.
			queries++;
			LerobotNestedQuery query(context, "SELECT count(*) FROM " + source);
			CheckResult(query.GetResult(), true);
			auto chunk = query.Fetch();
			row_count = chunk->GetValue(0, 0).GetValue<uint64_t>();
		}
		if (row_count > budget->limit / sizeof(IndexRow)) {
			return nullptr;
		}
		auto index = make_shared_ptr<TimestampIndex>(budget);
		if (!Reserve(*index, row_count * sizeof(IndexRow))) {
			return nullptr;
		}
		if (index->bytes) {
			try {
				index->storage = allocator.Allocate(index->bytes);
			} catch (const OutOfMemoryException &) {
				// Caching is optional. Free its reservation before running the
				// filtered query, which can still succeed with less memory.
				return nullptr;
			}
		}
		queries++;
		LerobotNestedQuery query(context,
		                         "SELECT CAST(episode_index AS BIGINT), CAST(frame_index AS BIGINT), "
		                         "CAST(timestamp AS DOUBLE) FROM " +
		                             source,
		                         true);
		CheckResult(query.GetResult(), true);
		auto rows = reinterpret_cast<IndexRow *>(index->storage.get());
		bool sorted = true;
		while (auto chunk = query.Fetch()) {
			UnifiedVectorFormat formats[3];
			for (idx_t col = 0; col < 3; col++) {
				chunk->data[col].ToUnifiedFormat(chunk->size(), formats[col]);
			}
			for (idx_t row = 0; row < chunk->size(); row++) {
				const auto ep = formats[0].sel->get_index(row);
				const auto frame = formats[1].sel->get_index(row);
				const auto ts = formats[2].sel->get_index(row);
				if (!formats[0].validity.RowIsValid(ep) || !formats[1].validity.RowIsValid(frame)) {
					continue; // NULL join keys never match a requested frame.
				}
				if (index->count == row_count) {
					return nullptr; // File grew between footer and column reads.
				}
				const bool has_timestamp = formats[2].validity.RowIsValid(ts);
				new (&rows[index->count])
				    IndexRow {formats[0].GetData<int64_t>()[ep], formats[1].GetData<int64_t>()[frame],
				              has_timestamp ? formats[2].GetData<double>()[ts] : 0, has_timestamp};
				if (index->count && rows[index->count].Key() < rows[index->count - 1].Key()) {
					sorted = false;
				}
				index->count++;
			}
		}
		CheckResult(query.GetResult(), true);
		if (!sorted) {
			idx_t comparisons = 0;
			std::sort(rows, rows + index->count, [&](const IndexRow &left, const IndexRow &right) {
				if (++comparisons % 8192 == 0) {
					CheckInterrupted(context);
				}
				return left.Key() < right.Key();
			});
		}
		CheckInterrupted(context);
		return index;
	}

	shared_ptr<TimestampIndex> GetIndex(ClientContext &context, const string &path) {
		if (budget->limit == 0) {
			return nullptr;
		}
		CheckInterrupted(context);
		shared_ptr<ShardSlot> slot;
		{
			lock_guard<mutex> guard(lock);
			auto found = slots.find(path);
			if (found == slots.end()) {
				if (slots.size() == MAX_TRACKED_SHARDS) {
					auto oldest =
					    std::min_element(slots.begin(), slots.end(), [](const SlotPair &a, const SlotPair &b) {
						    return a.second->last_use < b.second->last_use;
					    });
					slots.erase(oldest);
					evictions++;
				}
				slot = make_shared_ptr<ShardSlot>();
				slots.emplace(path, slot);
			} else {
				slot = found->second;
			}
			slot->last_use = ++clock;
			if (++slot->visits < INDEX_ON_VISIT || requests.load() < min_requests || slot->loading) {
				return nullptr;
			}
		}
		// Ineligible small requests do not add an open/stat/HEAD to the
		// native lookup. File identity is needed only for building or reuse.
		const auto fingerprint = ReadFingerprint(context, path);
		{
			lock_guard<mutex> guard(lock);
			auto current = slots.find(path);
			if (current == slots.end() || current->second != slot || slot->loading) {
				return nullptr;
			}
			if (slot->fingerprint && !(*slot->fingerprint == fingerprint)) {
				auto replacement = make_shared_ptr<ShardSlot>();
				replacement->fingerprint = make_uniq<Fingerprint>(fingerprint);
				replacement->visits = 1;
				replacement->last_use = slot->last_use;
				current->second = std::move(replacement);
				return nullptr;
			}
			if (slot->index) {
				index_hits++;
				return slot->index;
			}
			if (slot->skipped) {
				return nullptr;
			}
			slot->fingerprint = make_uniq<Fingerprint>(fingerprint);
			slot->loading = true;
		}
		try {
			// No lock or wait across a nested query. Concurrent callers use the
			// filtered lookup while this loader is running; they never wait on
			// a producer that could need the same DuckDB worker to make progress.
			shared_ptr<TimestampIndex> index;
			try {
				index = Load(context, path);
			} catch (const std::exception &error) {
				CheckInterrupted(context);
				const auto type = ErrorData(error).Type();
				if (type != ExceptionType::OUT_OF_MEMORY && type != ExceptionType::CONVERSION &&
				    type != ExceptionType::INVALID_INPUT && type != ExceptionType::IO) {
					throw;
				}
				// Reading an entire shard can encounter bad rows/pages which
				// the original filtered query never reads. Let that query decide
				// whether the requested rows are usable, and skip further index
				// attempts for this fingerprint. Never suppress an interrupt or
				// an internal/fatal error.
			}
			const bool unchanged = ReadFingerprint(context, path) == fingerprint;
			lock_guard<mutex> guard(lock);
			slot->loading = false;
			auto current = slots.find(path);
			if (!unchanged || current == slots.end() || current->second != slot) {
				return nullptr;
			}
			slot->skipped = !index;
			slot->index = index;
			if (index) {
				index_loads++;
				indexed_rows += index->count;
			}
			return index;
		} catch (...) {
			lock_guard<mutex> guard(lock);
			slot->loading = false;
			throw;
		}
	}

	vector<LerobotTimestampMatch> Lookup(ClientContext &context, const vector<string> &files,
	                                     const vector<LerobotTimestampKey> &keys, idx_t request_count) {
		CheckInterrupted(context);
		auto seen = requests.load();
		while (seen < min_requests &&
		       !requests.compare_exchange_weak(seen, seen + MinValue(min_requests - seen, request_count))) {
		}
		vector<LerobotTimestampMatch> matches(keys.size());
		if (keys.empty()) {
			return matches;
		}
		vector<string> uncached;
		for (const auto &file : files) {
			auto index = GetIndex(context, file);
			if (!index) {
				uncached.push_back(file);
				continue;
			}
			for (idx_t key = 0; key < keys.size(); key++) {
				Merge(matches[key], index->Find(keys[key]));
			}
			CheckInterrupted(context);
		}
		if (!uncached.empty()) {
			queries++;
			LerobotNestedQuery query(context, FilteredQuery(uncached, keys), true);
			CheckResult(query.GetResult());
			while (auto chunk = query.Fetch()) {
				UnifiedVectorFormat formats[4];
				for (idx_t col = 0; col < 4; col++) {
					chunk->data[col].ToUnifiedFormat(chunk->size(), formats[col]);
				}
				for (idx_t row = 0; row < chunk->size(); row++) {
					const auto ep = formats[0].GetData<int64_t>()[formats[0].sel->get_index(row)];
					const auto frame = formats[1].GetData<int64_t>()[formats[1].sel->get_index(row)];
					const auto ts = formats[2].sel->get_index(row);
					const auto key = std::lower_bound(keys.begin(), keys.end(), LerobotTimestampKey {ep, frame});
					D_ASSERT(key != keys.end() && *key == LerobotTimestampKey(ep, frame));
					LerobotTimestampMatch match;
					match.count = formats[3].GetData<int64_t>()[formats[3].sel->get_index(row)];
					match.has_timestamp = formats[2].validity.RowIsValid(ts);
					if (match.has_timestamp) {
						match.timestamp = formats[2].GetData<double>()[ts];
					}
					Merge(matches[key - keys.begin()], match);
				}
			}
			CheckResult(query.GetResult());
		}
		return matches;
	}

	using SlotPair = std::pair<const string, shared_ptr<ShardSlot>>;
	Allocator &allocator;
	shared_ptr<IndexBudget> budget;
	mutex lock;
	unordered_map<string, shared_ptr<ShardSlot>> slots;
	idx_t clock = 0;
	const idx_t min_requests;
	std::atomic<idx_t> requests {0};
	std::atomic<idx_t> queries {0}, index_loads {0}, index_hits {0}, evictions {0}, indexed_rows {0};
};

LerobotTimestampLookup::LerobotTimestampLookup(ClientContext &context)
    : LerobotTimestampLookup(context, BufferManager::GetBufferManager(context).GetMaxMemory() / 8, MIN_INDEX_REQUESTS) {
}

LerobotTimestampLookup::LerobotTimestampLookup(ClientContext &context, idx_t max_bytes, idx_t min_requests)
    : impl(make_uniq<Impl>(context, max_bytes, min_requests)) {
}

LerobotTimestampLookup::~LerobotTimestampLookup() {
}

vector<LerobotTimestampMatch> LerobotTimestampLookup::Lookup(ClientContext &context, const vector<string> &files,
                                                             const vector<LerobotTimestampKey> &keys,
                                                             idx_t request_count) {
	return impl->Lookup(context, files, keys, request_count ? request_count : keys.size());
}

LerobotTimestampLookupStats LerobotTimestampLookup::GetStats() const {
	return {impl->queries.load(),      impl->index_loads.load(),   impl->index_hits.load(),  impl->evictions.load(),
	        impl->indexed_rows.load(), impl->budget->bytes.load(), impl->budget->peak.load()};
}

} // namespace duckdb
