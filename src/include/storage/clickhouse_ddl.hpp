#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class ClientContext;
class ClickhouseCatalog;
class ClickhouseTableEntry;
struct ClickhouseColumnInfo;
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
	//! enable_time_time64_type=1, then clears the catalog's cache -- also when the statement failed. The cleared
	//! entries are retired and stay valid until the current transaction ends (see ClickhouseTransactionManager);
	//! methods of a cached schema entry also hold catalog.GetSchemaEntryOwner(<their name>) across the call
	//! (belt-and-braces)
	static void Execute(ClientContext &context, ClickhouseCatalog &catalog, const string &sql);
	//! The (freshly loaded) table entry, or null
	static optional_ptr<ClickhouseTableEntry> LookupTable(ClientContext &context, ClickhouseCatalog &catalog,
	                                                      const string &database, const string &table);
	//! Creates the table (CreateTableSql + Execute) and returns its freshly loaded entry. With IF NOT EXISTS and
	//! an existing table, returns the existing table's entry
	static ClickhouseTableEntry &CreateTable(ClientContext &context, ClickhouseCatalog &catalog,
	                                         const string &database, CreateTableInfo &info);
	//! ALTER TABLE ADD COLUMN [IF NOT EXISTS] / DROP COLUMN [IF EXISTS] / RENAME COLUMN, or RENAME TABLE; anything
	//! else throws NotImplementedException. Column names are resolved against the table's ClickHouse columns the
	//! way DuckDB resolves identifiers, case-insensitively (see ResolveColumn()), and the statement uses the real
	//! ClickHouse name. Returns an empty string when there is nothing to send: ADD COLUMN IF NOT EXISTS of a column
	//! that exists, DROP COLUMN IF EXISTS of one that does not
	static string AlterTableSql(ClientContext &context, const string &database, const ClickhouseTableEntry &table,
	                            AlterTableInfo &info);
	//! The ClickHouse column of `table` that the DuckDB identifier `name` refers to: the exact (case-sensitive) match,
	//! else the only case-insensitive one. Null if there is none; CatalogException if several columns match only
	//! case-insensitively
	static optional_ptr<const ClickhouseColumnInfo> ResolveColumn(const ClickhouseTableEntry &table,
	                                                              const string &name);
};

} // namespace duckdb
