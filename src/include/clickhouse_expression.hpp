#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/value.hpp"

#include <functional>

namespace duckdb {
class Expression;
class TableFilter;

//! Translates bound DuckDB expressions into ClickHouse SQL through an allow-list (UPDATE/DELETE predicates and SET
//! values). Throws NotImplementedException for anything outside it: no expression is ever partially translated
class ClickhouseExpression {
public:
	//! `resolve_reference` maps a BoundReferenceExpression index (into the output of the operator the expression
	//! is evaluated over) to ClickHouse SQL, e.g. a quoted column name
	static string Translate(const Expression &expr, const std::function<string(idx_t)> &resolve_reference);
	//! A constant as a ClickHouse literal. Throws NotImplementedException for types without one
	static string Literal(const Value &value);
	//! `sql` (ClickHouse SQL for a value of DuckDB type `source`) converted to `clickhouse_type` (ClickHouse's form
	//! of DuckDB type `target`) with DuckDB's result, e.g. CAST(roundBankers(x) AS Int32) for DOUBLE to INTEGER,
	//! where ClickHouse's CAST alone truncates. Throws NotImplementedException for a cast without an exact
	//! translation (e.g. DOUBLE or VARCHAR to DECIMAL, time-zone-dependent casts, most casts to VARCHAR)
	static string Cast(const string &sql, const LogicalType &source, const LogicalType &target,
	                   const string &clickhouse_type);
	//! A filter DuckDB pushed into a scan (LogicalGet::table_filters) on `column` (ClickHouse SQL for a column of
	//! DuckDB type `column_type`), translated exactly through Translate(). Never narrows a predicate by dropping a
	//! part: returns "" only for an OPTIONAL_FILTER (a hint: DuckDB keeps its exact predicate in a LogicalFilter
	//! above the scan), which the caller skips; throws NotImplementedException for anything it cannot translate
	//! exactly, including runtime (dynamic, bloom) filters and an OR with a non-exact branch
	static string TranslateTableFilter(const TableFilter &filter, const LogicalType &column_type, const string &column);
};

} // namespace duckdb
