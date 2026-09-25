#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/common/index_vector.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class ClickhouseCatalog;
class ClickhouseTableEntry;
class ClickhouseInsertGlobalState;
struct CreateTableInfo;

//! One column an INSERT writes: the ClickHouse column, and the input chunk column holding its values
struct ClickhouseInsertColumn {
	ClickhouseColumnInfo column;
	idx_t source_index;
};

//! INSERT, INSERT … SELECT, COPY … FROM and CREATE TABLE … AS SELECT into a ClickHouse table: streams the input into
//! one native INSERT on one pooled connection, a block every ch_insert_block_size rows
class ClickhouseInsert : public PhysicalOperator {
public:
	ClickhouseInsert(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseTableEntry &table,
	                 vector<ClickhouseInsertColumn> columns);

	//! CREATE TABLE … AS SELECT: the table is created when the statement runs (on the first row, or in Finalize
	//! when the query yields none), then filled like an INSERT listing every column
	ClickhouseInsert(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseCatalog &catalog,
	                 const string &database, unique_ptr<CreateTableInfo> create_info);

	//! The attached database (DuckDB catalog) name, resolved again when the INSERT runs. No reference to the table
	//! entry or the catalog is kept: a ClearCache() (DDL, clickhouse_clear_cache(), clickhouse_execute(), from any
	//! connection) between planning and execution retires the entry, which then only lives until the transaction
	//! ends -- a prepared INSERT is planned in one transaction and may run in another -- and a DETACH can free the
	//! catalog
	string catalog_name;
	//! The ClickHouse database and table names, for EXPLAIN and errors
	string database_name;
	string table_name;
	//! The INSERT target resolved at plan time: the inserted columns, in table order. Empty for CTAS, whose target
	//! is only known once the table is created (see ClickhouseInsertGlobalState::columns)
	vector<ClickhouseInsertColumn> columns;
	//! The INSERT target resolved at plan time: the statement BeginInsert() runs, e.g.
	//! INSERT INTO `db`.`t` (`a`, `b`) VALUES. Empty for CTAS, see `columns`
	string insert_sql;
	//! CTAS only: the table to create
	unique_ptr<CreateTableInfo> create_info;

	//! The columns an INSERT with this column_index_map writes (every column when the map is empty), in table order.
	//! Throws for columns that cannot be inserted into
	static vector<ClickhouseInsertColumn> GetInsertColumns(ClickhouseTableEntry &table,
	                                                       const physical_index_vector_t<idx_t> &column_index_map);
	static string BuildInsertQuery(ClickhouseTableEntry &table, const vector<ClickhouseInsertColumn> &columns);

	//! Starts the ClickHouse INSERT: resolves the attached database again, takes a connection through
	//! ClickhouseCatalog::StartWrite() and sends the INSERT statement. Called on the first row, not when the
	//! sink state is created, which DuckDB does when it schedules the query -- possibly long before any upstream
	//! ORDER BY, aggregate or join yields a row, which could outlast the server's receive_timeout
	void StartInsert(ClientContext &context, ClickhouseInsertGlobalState &gstate) const;

public:
	// Source interface
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
