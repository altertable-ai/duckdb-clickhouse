#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ClickhouseClearCacheFunction : public TableFunction {
public:
	ClickhouseClearCacheFunction();

	//! Drops the cached databases, tables and columns of every attached ClickHouse database
	static void ClearClickhouseCaches(ClientContext &context);
};

} // namespace duckdb
