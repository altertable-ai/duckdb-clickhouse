#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

class ClickhouseStorageExtension : public StorageExtension {
public:
	ClickhouseStorageExtension();
};

} // namespace duckdb
