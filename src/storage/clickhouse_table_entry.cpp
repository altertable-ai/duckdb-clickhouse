#include "storage/clickhouse_table_entry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

ClickhouseTableEntry::ClickhouseTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                           vector<ClickhouseColumnInfo> columns, optional_idx approx_rows)
    : TableCatalogEntry(catalog, schema, info), clickhouse_columns(std::move(columns)), approx_rows(approx_rows) {
}

unique_ptr<BaseStatistics> ClickhouseTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction ClickhouseTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	// replaced in Task 6
	throw NotImplementedException("Scanning ClickHouse tables is not implemented yet");
}

TableStorageInfo ClickhouseTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	if (approx_rows.IsValid()) {
		result.cardinality = approx_rows.GetIndex();
	}
	return result;
}

} // namespace duckdb
