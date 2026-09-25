#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! clickhouse_type_mapping('<ClickHouse type>'): shows how a ClickHouse type is mapped and read
class ClickhouseTypeMappingFunction : public TableFunction {
public:
	ClickhouseTypeMappingFunction();
};

} // namespace duckdb
