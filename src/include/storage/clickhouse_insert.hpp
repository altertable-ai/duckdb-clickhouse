#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/common/index_vector.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class ClickhouseTableEntry;
class ClickhouseInsertGlobalState;

//! One column an INSERT writes: the ClickHouse column, and the input chunk column holding its values
struct ClickhouseInsertColumn {
	ClickhouseColumnInfo column;
	idx_t source_index;
};

//! INSERT, INSERT … SELECT and COPY … FROM into a ClickHouse table: streams the input into one native INSERT on one
//! pooled connection, a block every ch_insert_block_size rows
class ClickhouseInsert : public PhysicalOperator {
public:
	ClickhouseInsert(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseTableEntry &table,
	                 vector<ClickhouseInsertColumn> columns);

	//! The attached database (DuckDB catalog) name, resolved again when the INSERT runs. No reference to the table
	//! entry or the catalog is kept: the entry is freed by a ClearCache() (clickhouse_clear_cache(),
	//! clickhouse_execute(), from any connection) that can happen between planning and execution, and a
	//! DETACH can do the same to the catalog
	string catalog_name;
	//! The ClickHouse database and table names, for EXPLAIN and errors
	string database_name;
	string table_name;
	//! The inserted columns, in table order
	vector<ClickhouseInsertColumn> columns;
	//! The statement BeginInsert() runs, e.g. INSERT INTO `db`.`t` (`a`, `b`) VALUES
	string insert_sql;

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
