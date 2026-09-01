#pragma once

#include "duckdb/main/extension.hpp"

namespace duckdb {

class LerobotExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	string Name() override;
};

} // namespace duckdb
