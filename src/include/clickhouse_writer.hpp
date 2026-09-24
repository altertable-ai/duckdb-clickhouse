#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/common/types/vector.hpp"

#include <clickhouse/columns/column.h>

namespace duckdb {

//! How values of a ClickHouse column are written
enum class ClickhouseWriteMode : uint8_t {
	//! Encoded by ClickhouseWriter in the column's own native representation
	NATIVE,
	//! Sent in the form the read path produces (text, or Float32 for BFloat16) and converted by ClickHouse:
	//! IPv4/6, (U)Int256, Decimal(P > 38), BFloat16, JSON and the geo types
	SERVER_CONVERSION,
	//! Cannot be written: Variant, Dynamic, Object, AggregateFunction, SimpleAggregateFunction, Nothing, and
	//! Array/Tuple/Map holding anything that is not NATIVE
	UNSUPPORTED
};

//! Converts DuckDB vectors into clickhouse-cpp columns for INSERTs: the mirror image of ClickhouseConversion
class ClickhouseWriter {
public:
	static ClickhouseWriteMode GetWriteMode(const ClickhouseTypeNode &node);
	//! Appends the first `count` rows of `source` to `target`, a column cloned from an INSERT header block.
	//! `column_name` names the column in errors. Throws ConversionException for values ClickHouse cannot store:
	//! out-of-range dates and timestamps, oversized FixedStrings, NULLs in non-Nullable columns
	static void AppendVector(Vector &source, idx_t count, const clickhouse::ColumnRef &target,
	                         const string &column_name);
};

} // namespace duckdb
