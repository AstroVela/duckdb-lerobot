#include "lerobot_path.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/encryption_key_manager.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

namespace {

bool IsQualifiedRoot(FileSystem &fs, const string &root) {
	return fs.IsPathAbsolute(root) || StringUtil::Contains(root, "://");
}

string GetFileSearchPath(ClientContext &context) {
	Value value;
	if (!context.TryGetCurrentSetting("file_search_path", value) || value.IsNull()) {
		return string();
	}
	return StringValue::Get(value);
}

void AppendCacheComponent(string &key, const string &value) {
	key += to_string(value.size()) + ":" + value;
}

string CacheDigest(const string &input) {
	// Use DuckDB's SHA-256 implementation without retaining credentials in a
	// cache key. This utility prefixes a fixed-size salt before hashing.
	data_t salt[MainHeader::DB_IDENTIFIER_LEN] = {};
	data_t digest[EncryptionKeyManager::DERIVED_KEY_LENGTH];
	EncryptionKeyManager::KeyDerivationFunctionSHA256(const_data_ptr_cast(input.data()), input.size(), salt, digest);
	static constexpr char HEX[] = "0123456789abcdef";
	string result;
	for (const auto byte : digest) {
		result += HEX[byte >> 4];
		result += HEX[byte & 15];
	}
	return result;
}

string S3StorageCacheKey(ClientContext &context, const string &root) {
	if (!StringUtil::StartsWith(root, "s3://") && !StringUtil::StartsWith(root, "s3a://") &&
	    !StringUtil::StartsWith(root, "s3n://") && !StringUtil::StartsWith(root, "r2://") &&
	    !StringUtil::StartsWith(root, "gcs://") && !StringUtil::StartsWith(root, "gs://")) {
		return string();
	}
	// A public info.json fingerprint proves neither storage identity nor access
	// to episode metadata. Fingerprint HTTPFS's S3 settings and every secret whose
	// scope overlaps this dataset, including scopes below meta/episodes/. Looking
	// up only the manifest's winning secret misses file-specific overrides.
	string key = "lerobot_s3_access_v1";
	for (const auto name : {"s3_endpoint", "s3_region", "s3_url_style", "s3_use_ssl", "s3_access_key_id",
	                        "s3_secret_access_key", "s3_session_token", "s3_kms_key_id", "s3_requester_pays",
	                        "s3_url_compatibility_mode", "enable_global_s3_configuration"}) {
		Value value;
		auto setting = context.TryGetCurrentSetting(name, value);
		AppendCacheComponent(key, setting ? to_string(static_cast<uint8_t>(setting.GetScope())) : "unset");
		AppendCacheComponent(key, value.ToSQLString());
	}
	auto &manager = SecretManager::Get(context);
	auto secrets = manager.AllSecrets(CatalogTransaction::GetSystemCatalogTransaction(context));
	vector<string> entries;
	for (const auto &entry : secrets) {
		const auto &secret = *entry.secret;
		const auto &type = secret.GetType();
		if (type != "s3" && type != "r2" && type != "gcs" && type != "aws") {
			continue;
		}
		bool overlaps = false;
		for (const auto &scope : secret.GetScope()) {
			overlaps = overlaps || StringUtil::StartsWith(root, scope) || StringUtil::StartsWith(scope, root + "/");
		}
		if (!overlaps) {
			continue;
		}
		string encoded;
		for (const auto &part : {entry.storage_mode, type, secret.GetName(), secret.GetProvider()}) {
			AppendCacheComponent(encoded, part);
		}
		AppendCacheComponent(encoded, to_string(secret.GetScope().size()));
		for (const auto &scope : secret.GetScope()) {
			AppendCacheComponent(encoded, scope);
		}
		const auto &values = dynamic_cast<const KeyValueSecret &>(secret).secret_map;
		for (const auto &kv : values) {
			AppendCacheComponent(encoded, kv.first);
			AppendCacheComponent(encoded, kv.second.type().ToString());
			AppendCacheComponent(encoded, kv.second.ToSQLString());
		}
		entries.push_back(CacheDigest(encoded));
	}
	std::sort(entries.begin(), entries.end());
	for (const auto &entry : entries) {
		AppendCacheComponent(key, entry);
	}
	return ",s3_access=" + CacheDigest(key);
}

bool IsPortablePathComponent(const string &component) {
	// Reject Windows path/stream syntax, glob expansion and URI delimiters.
	// Percent escapes must not introduce separators or dot segments remotely.
	if (component.empty() || component == "." || component == ".." || component.back() == '.' ||
	    component.back() == ' ' || component.find_first_of("/\\:*?\"<>|[]%#") != string::npos) {
		return false;
	}
	for (const auto ch : component) {
		if (static_cast<unsigned char>(ch) < 32 || ch == 127) {
			return false;
		}
	}
	return true;
}

} // namespace

string NormalizeLerobotRoot(string root) {
	// Preserve non-ASCII path bytes: DuckDB 1.5's Trim treats signed UTF-8
	// bytes as whitespace. Only remove the ASCII whitespace accepted here.
	const auto first = root.find_first_not_of(" \f\n\r\t\v");
	if (first == string::npos) {
		throw BinderException("LeRobot dataset root must not be empty");
	}
	const auto last = root.find_last_not_of(" \f\n\r\t\v");
	root = root.substr(first, last - first + 1);
	while (root.size() > 1 && root.back() == '/') {
		root.pop_back();
	}
	if (root.empty()) {
		throw BinderException("LeRobot dataset root must not be empty");
	}
	return root;
}

string ResolveLerobotRoot(ClientContext &context, const string &root) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto expanded = fs.ExpandPath(NormalizeLerobotRoot(root));
	if (IsQualifiedRoot(fs, expanded)) {
		return expanded;
	}
	const auto local_root = fs.JoinPath(FileSystem::GetWorkingDirectory(), expanded);
	if (GetFileSearchPath(context).empty()) {
		return local_root;
	}
	// Native non-glob lookup prefers the working directory, then returns all
	// matches in file_search_path. A dataset must resolve to one info.json so
	// its metadata and shards cannot be combined across search directories.
	auto matches = fs.GlobFiles(expanded + "/meta/info.json", FileGlobOptions::ALLOW_EMPTY);
	if (matches.empty()) {
		return local_root; // Let the reader report its normal missing-file error.
	}
	string resolved;
	for (const auto &match : matches) {
		auto path = fs.ExpandPath(match.path);
		if (!IsQualifiedRoot(fs, path)) {
			path = fs.JoinPath(FileSystem::GetWorkingDirectory(), path);
		}
		auto candidate = StringUtil::GetFilePath(StringUtil::GetFilePath(path));
		if (!resolved.empty() && candidate != resolved) {
			throw BinderException("LeRobot dataset root '%s' matches multiple datasets in file_search_path; "
			                      "use an explicit path",
			                      root);
		}
		resolved = std::move(candidate);
	}
	return resolved;
}

string LerobotRootCacheKey(ClientContext &context, const string &root) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto expanded = fs.ExpandPath(NormalizeLerobotRoot(root));
	if (IsQualifiedRoot(fs, expanded)) {
		return Value(expanded).ToSQLString() + S3StorageCacheKey(context, expanded);
	}
	// Quote each component so roots and search lists cannot collide. No storage
	// lookup is needed: Peek describes the last cached result, not freshness.
	auto key = Value(fs.JoinPath(FileSystem::GetWorkingDirectory(), expanded)).ToSQLString();
	const auto search_path = GetFileSearchPath(context);
	if (!search_path.empty()) {
		for (const auto &path : StringUtil::Split(search_path, ',')) {
			key += "," + Value(fs.ExpandPath(path)).ToSQLString();
		}
	}
	return key;
}

string ResolveLerobotRelativePath(const string &root, const string &relative_path, const char *path_name) {
	idx_t begin = 0;
	while (true) {
		const auto end = relative_path.find('/', begin);
		if (!IsPortablePathComponent(relative_path.substr(begin, end == string::npos ? end : end - begin))) {
			throw BinderException(
			    "LeRobot %s must stay relative to the dataset root and use portable path components: '%s'", path_name,
			    relative_path);
		}
		if (end == string::npos) {
			break;
		}
		begin = end + 1;
	}
	return root + "/" + relative_path;
}

void ValidateLerobotFeatureName(const string &name) {
	if (!IsPortablePathComponent(name)) {
		throw BinderException("LeRobot feature name must be a portable path component: '%s'", name);
	}
}

} // namespace duckdb
