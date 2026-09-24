#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

//! DuckDB column type -> ClickHouse type, for CREATE TABLE and ALTER TABLE ADD COLUMN: the reverse of
//! ClickhouseTypes::ToDuckDB, chosen so that reading the column back maps to the same (or, for BLOB and the naive
//! TIMESTAMP family, the documented) DuckDB type
class ClickhouseDdlTypes {
public:
	//! `nullable` wraps scalar types in Nullable(). Array, Tuple, Map and JSON cannot be Nullable in ClickHouse and
	//! are never wrapped. Nested element and field types are always nullable (DuckDB LIST/STRUCT/MAP values can hold
	//! NULLs), except Map keys. Throws NotImplementedException for types without a ClickHouse equivalent
	static string ToClickhouse(const LogicalType &type, bool nullable);
};

} // namespace duckdb
