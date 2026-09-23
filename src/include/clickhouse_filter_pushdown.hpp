#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

class ClickhouseFilterPushdown {
public:
	//! WHERE clause body for the filters DuckDB pushed into the scan (empty when nothing restricts the rows).
	//! filters is keyed by position in column_ids; column_ids index into columns.
	static string TransformFilters(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
	                               const vector<ClickhouseColumnInfo> &columns);
	//! Translates one filter on `column`. Returns "" when the filter does not restrict rows (dynamic/bloom filters,
	//! or optional filters that cannot be translated). Throws for required filters that cannot be translated.
	static string TransformFilter(const string &column, const TableFilter &filter, bool optional = false);
	static string TransformConstant(const Value &value);
};

} // namespace duckdb
