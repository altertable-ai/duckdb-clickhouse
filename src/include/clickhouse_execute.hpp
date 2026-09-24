#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! clickhouse_execute('<attached database>', '<ClickHouse statement>'): runs a statement that returns no rows
class ClickhouseExecuteFunction : public TableFunction {
public:
	ClickhouseExecuteFunction();
};

} // namespace duckdb
