#pragma once

#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class ClickhouseTableEntry;
class LogicalDelete;
class TableCatalogEntry;

//! A DELETE / TRUNCATE / UPDATE translated into ClickHouse SQL
struct ClickhouseDmlStatement {
	//! The attached database (DuckDB catalog), resolved again when the statement runs
	string catalog_name;
	//! For errors and EXPLAIN, e.g. DELETE FROM `db`.`t`
	string description;
	//! SELECT count() FROM `db`.`t` [WHERE p]: the affected-row count
	string count_sql;
	string sql;
	//! Query-level settings sent with `sql`, e.g. lightweight_deletes_sync
	vector<std::pair<string, string>> settings;
};

//! The modified table and the exact ClickHouse WHERE predicate of an UPDATE/DELETE plan
struct ClickhouseDmlTarget {
	ClickhouseTableEntry &table;
	//! `db`.`t`
	string qualified_name;
	//! Empty: every row
	string predicate;
};

//! Translates the logical plan of an UPDATE/DELETE on an attached ClickHouse table into one ClickHouse statement.
//! Runs from the catalog's logical-level Plan* hooks, i.e. during physical planning, after ColumnBindingResolver has
//! turned every column reference below the DML operator into a BoundReferenceExpression indexing its child
//! operator's output, and before the child itself is planned
class ClickhouseDml {
public:
	//! Checks the plan below a LogicalDelete/LogicalUpdate: [LogicalProjection | LogicalFilter]* ending at a
	//! ClickHouse scan of `table`. Collects the exact predicate. Throws NotImplementedException ("<statement> on
	//! ClickHouse tables must filter only the modified table with translatable expressions (…); …") otherwise
	static ClickhouseDmlTarget AnalyzeTarget(const string &statement, TableCatalogEntry &table,
	                                         LogicalOperator &child);
	//! ClickHouse SQL for output column `index` of `op` (a LogicalGet, LogicalFilter or LogicalProjection)
	static string ResolveOutput(const LogicalOperator &op, idx_t index);
	static ClickhouseDmlStatement PlanDelete(LogicalDelete &op);
	//! Throws the "must filter only the modified table" error for `statement`, with `reason`
	[[noreturn]] static void ThrowUnsupportedShape(const string &statement, const string &reason);
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
