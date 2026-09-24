#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

class ClickhouseTableEntry : public TableCatalogEntry {
public:
	ClickhouseTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	                     vector<ClickhouseColumnInfo> columns, optional_idx approx_rows);

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	const vector<ClickhouseColumnInfo> &GetClickhouseColumns() const {
		return clickhouse_columns;
	}
	optional_idx GetApproxRows() const {
		return approx_rows;
	}

private:
	vector<ClickhouseColumnInfo> clickhouse_columns;
	//! system.tables.total_rows (not known for views and some engines)
	optional_idx approx_rows;
};

} // namespace duckdb
