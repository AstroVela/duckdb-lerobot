#include "lerobot_path.hpp"

#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

string NormalizeLerobotRoot(string root) {
	StringUtil::Trim(root);
	while (root.size() > 1 && root.back() == '/') {
		root.pop_back();
	}
	if (root.empty()) {
		throw BinderException("LeRobot dataset root must not be empty");
	}
	return root;
}

} // namespace duckdb
