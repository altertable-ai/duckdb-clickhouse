#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class ClientContext;
class ClickhouseCatalog;
class ClickhouseTableEntry;
class ColumnDefinition;
struct AlterTableInfo;
struct CreateTableInfo;

//! Builds and runs the ClickHouse DDL for DuckDB DDL statements
class ClickhouseDdl {
public:
	//! CREATE [OR REPLACE] TABLE [IF NOT EXISTS] `database`.`table` (…) ENGINE = <ch_default_table_engine>
	//! [ORDER BY (<primary key>) | ORDER BY tuple()]. Throws NotImplementedException for what ClickHouse tables
	//! cannot express (see the Global Constraints of the Phase 2 plan)
	static string CreateTableSql(ClientContext &context, const string &database, CreateTableInfo &info);
	//! `name` <type> [DEFAULT <constant>]
	static string ColumnSql(ClientContext &context, const ColumnDefinition &column, bool nullable);
	//! " DEFAULT <literal>" for a column with a constant DEFAULT, "" without one
	static string DefaultValueSql(ClientContext &context, const ColumnDefinition &column);
	//! ch_default_table_engine, validated
	static string TableEngine(ClientContext &context);
	//! Throws InvalidInputException unless `engine` is an identifier optionally followed by one (…) group ending
	//! the string
	static void ValidateEngine(const string &engine);
	//! Runs a DDL statement through catalog.StartWrite() (READ_ONLY check, marks the transaction written) with
	//! enable_time_time64_type=1, then clears the catalog's cache -- also when the statement failed. Callers that are
	//! methods of a cached schema entry must hold catalog.GetSchemaEntryOwner(<their name>) across the call
	static void Execute(ClientContext &context, ClickhouseCatalog &catalog, const string &sql);
	//! The (freshly loaded) table entry, or null
	static optional_ptr<ClickhouseTableEntry> LookupTable(ClientContext &context, ClickhouseCatalog &catalog,
	                                                      const string &database, const string &table);
	//! Creates the table (CreateTableSql + Execute) and returns its freshly loaded entry. With IF NOT EXISTS and
	//! an existing table, returns the existing table's entry
	static ClickhouseTableEntry &CreateTable(ClientContext &context, ClickhouseCatalog &catalog,
	                                         const string &database, CreateTableInfo &info);
	//! ALTER TABLE ADD COLUMN [IF NOT EXISTS] / DROP COLUMN [IF EXISTS] / RENAME COLUMN, or RENAME TABLE; anything
	//! else throws NotImplementedException
	static string AlterTableSql(ClientContext &context, const string &database, const string &table,
	                            AlterTableInfo &info);
};

} // namespace duckdb
