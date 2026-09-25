#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class ClickhouseTableEntry;
class LogicalDelete;
class LogicalUpdate;
class TableCatalogEntry;

//! A DELETE / TRUNCATE / UPDATE translated into ClickHouse SQL
struct ClickhouseDmlStatement {
	//! The attached database (DuckDB catalog), resolved again when the statement runs
	string catalog_name;
	//! For errors and EXPLAIN, e.g. DELETE FROM `db`.`t`
	string description;
	//! SELECT count() FROM `db`.`t` [WHERE p]: the affected-row count. Empty when no row can match (e.g.
	//! WHERE 1 = 0): the statement then reports 0 without connecting to ClickHouse
	string count_sql;
	//! Empty when there is nothing to run
	string sql;
	//! Query-level settings sent with `sql`: ClickhouseDml::SemanticSettings(), plus lightweight_deletes_sync or
	//! mutations_sync
	vector<std::pair<string, string>> settings;
	//! The plan holds prepared-statement parameters without values (PREPARE): nothing was translated and running it
	//! throws. EXECUTE binds the statement again with the values as constants and plans it anew (the catalog reports
	//! no catalog version, so DuckDB always rebinds, see PreparedStatementData::RequireRebind)
	bool unbound_parameters = false;
};

//! The modified table and the exact ClickHouse WHERE predicate of an UPDATE/DELETE plan
struct ClickhouseDmlTarget {
	ClickhouseTableEntry &table;
	//! `db`.`t`
	string qualified_name;
	//! Empty: every row (unless matches_nothing or unbound_parameters is set)
	string predicate;
	//! DuckDB folded the plan to an empty result (e.g. WHERE 1 = 0, WHERE NULL): no row matches
	bool matches_nothing = false;
	//! The plan holds prepared-statement parameters without values: nothing was translated (see
	//! ClickhouseDmlStatement::unbound_parameters)
	bool unbound_parameters = false;
};

//! Translates the logical plan of an UPDATE/DELETE on an attached ClickHouse table into one ClickHouse statement.
//! Runs from the catalog's logical-level Plan* hooks, i.e. during physical planning, after ColumnBindingResolver has
//! turned every column reference below the DML operator into a BoundReferenceExpression indexing its child
//! operator's output, and before the child itself is planned
class ClickhouseDml {
public:
	//! Checks the plan below a LogicalDelete/LogicalUpdate: [LogicalProjection | LogicalFilter | IN-list MARK join]*
	//! ending at a ClickHouse scan of `table` (or at a LogicalEmptyResult: matches_nothing). Collects the exact
	//! predicate. Throws NotImplementedException ("<statement> on ClickHouse table "db"."t" must filter only the
	//! modified table with translatable expressions (…); …") otherwise.
	//!
	//! An IN-list MARK join is what DuckDB's InClauseRewriter makes of `x [NOT] IN (<5 or more constants>)`: a MARK
	//! join of the input with a LogicalColumnDataGet of the constants on `x = <constant column>`, whose mark column a
	//! LogicalFilter above uses as its whole expression (IN) or under a NOT (NOT IN). It translates through
	//! ClickhouseExpression::InList; any deviation from exactly that shape rejects the statement
	static ClickhouseDmlTarget AnalyzeTarget(const string &statement, TableCatalogEntry &table,
	                                         LogicalOperator &child);
	//! ClickHouse SQL for output column `index` of `op` (a LogicalGet, LogicalFilter, LogicalProjection or an IN-list
	//! MARK join, whose mark column itself is only accepted where AnalyzeTarget translates it)
	static string ResolveOutput(const LogicalOperator &op, idx_t index);
	static ClickhouseDmlStatement PlanDelete(LogicalDelete &op);
	//! ALTER TABLE `db`.`t` UPDATE c = CAST(e AS <c's ClickHouse type>), … WHERE p (WHERE 1 without a filter), sent
	//! with mutations_sync = ch_mutations_sync
	static ClickhouseDmlStatement PlanUpdate(ClientContext &context, LogicalUpdate &op);
	//! Throws the "<statement> on ClickHouse table "db"."t" must filter only the modified table" error, with `reason`
	[[noreturn]] static void ThrowUnsupportedShape(const string &statement, const TableCatalogEntry &table,
	                                               const string &reason);
	//! "db"."t", for errors
	static string DisplayName(const TableCatalogEntry &table);
	//! Query settings the count and the statement both run with, whatever the ATTACH's settings= holds
	//! (transform_null_in = 0). Only the count depends on them: the translated predicate does not (see InList)
	static vector<std::pair<string, string>> SemanticSettings();
};

//! Runs a ClickhouseDmlStatement once: the count, then the statement, on one write connection. Emits the count
class ClickhouseDmlOperator : public PhysicalOperator {
public:
	ClickhouseDmlOperator(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseDmlStatement statement);

	ClickhouseDmlStatement statement;

	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}
	bool ParallelSource() const override {
		return false;
	}
	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
