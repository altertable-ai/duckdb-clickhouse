#include "storage/clickhouse_table_entry.hpp"

#include "clickhouse_scanner.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "storage/clickhouse_catalog.hpp"

namespace duckdb {

ClickhouseTableEntry::ClickhouseTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                           vector<ClickhouseColumnInfo> columns, optional_idx approx_rows)
    : TableCatalogEntry(catalog, schema, info), clickhouse_columns(std::move(columns)), approx_rows(approx_rows) {
}

unique_ptr<BaseStatistics> ClickhouseTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction ClickhouseTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<ClickhouseScanBindData>();
	result->pool = catalog.Cast<ClickhouseCatalog>().GetConnectionPoolPtr();
	result->database = schema.name;
	result->table = name;
	result->columns = clickhouse_columns;
	result->approx_rows = approx_rows;
	bind_data = std::move(result);
	return ClickhouseScanFunction();
}

TableStorageInfo ClickhouseTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	if (approx_rows.IsValid()) {
		result.cardinality = approx_rows.GetIndex();
	}
	return result;
}

} // namespace duckdb
