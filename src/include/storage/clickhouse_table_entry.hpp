#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

class ClickhouseTableEntry : public TableCatalogEntry {
public:
	//! `info` holds the DuckDB columns; `columns` every ClickHouse column. They are the same list unless
	//! `column_collision` is set (see below)
	ClickhouseTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	                     vector<ClickhouseColumnInfo> columns, optional_idx approx_rows, string engine,
	                     string column_collision);

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	//! Every ClickHouse column, in table order. Matches the DuckDB column list one to one unless the table has
	//! case-colliding columns (see ThrowIfColumnsCollide())
	const vector<ClickhouseColumnInfo> &GetClickhouseColumns() const {
		return clickhouse_columns;
	}
	//! Throws InvalidInputException if the table has columns whose names differ only in case, which DuckDB
	//! (case-insensitive identifiers) cannot tell apart. Such a table (created outside DuckDB) is still listed, with
	//! the first column of each colliding group only, so it does not break its whole database; scanning or
	//! inserting into it fails here with this error. ALTER TABLE still works on it, e.g. to rename a column
	void ThrowIfColumnsCollide() const;
	optional_idx GetApproxRows() const {
		return approx_rows;
	}
	const string &GetEngine() const {
		return engine;
	}

private:
	vector<ClickhouseColumnInfo> clickhouse_columns;
	//! system.tables.total_rows (not known for views and some engines)
	optional_idx approx_rows;
	//! system.tables.engine, e.g. MergeTree, View, Dictionary
	string engine;
	//! Set when two ClickHouse columns' names differ only in case, e.g. "id" and "ID" (see ThrowIfColumnsCollide())
	string column_collision;
};

} // namespace duckdb
