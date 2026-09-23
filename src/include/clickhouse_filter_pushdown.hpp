#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

class ClickhouseFilterPushdown {
public:
	//! WHERE clause body for the filters DuckDB pushed into the scan (empty when nothing restricts the rows).
	//! filters is keyed by position in column_ids; column_ids index into columns.
	//!
	//! filters must be the *static* filter set -- PhysicalTableScan::table_filters (see ClickhouseInitGlobal),
	//! not TableFunctionInitInput::filters directly: the latter is that same static set merged, at scan-init
	//! time, with whatever a join's or TopN's runtime "dynamic" filter mechanism has attached to the scan by
	//! then, which can add a filter indistinguishable by type from a real predicate but meant only as a pruning
	//! hint, never required for correctness (see task-8 fix round 1) -- translating it here as if it were makes
	//! ClickHouse apply it eagerly, before whatever operator (the join, or a LIMIT above it) is supposed to see
	//! the unfiltered rows first.
	static string TransformFilters(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
	                               const vector<ClickhouseColumnInfo> &columns);
	//! Translates one filter on `column`. Returns "" for a filter that must not become a ClickHouse predicate:
	//! TableFilterType::OPTIONAL_FILTER (by DuckDB's own definition, not required for query correctness -- e.g.
	//! a join- or TopN-pushed narrowing that DuckDB still applies itself above the scan; applying it inside
	//! ClickHouse too could exclude rows before the join/ORDER BY/LIMIT above the scan sees them) and
	//! DYNAMIC_FILTER/BLOOM_FILTER (runtime hints enforced by the operator that created them, not by the scan).
	//! Throws for a *required* filter that cannot be translated -- it never silently drops one.
	static string TransformFilter(const string &column, const TableFilter &filter);
	static string TransformConstant(const Value &value);
};

} // namespace duckdb
