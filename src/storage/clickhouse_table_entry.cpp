#include "storage/clickhouse_table_entry.hpp"

#include "clickhouse_scanner.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "storage/clickhouse_catalog.hpp"

namespace duckdb {

ClickhouseTableEntry::ClickhouseTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                           vector<ClickhouseColumnInfo> columns, optional_idx approx_rows,
                                           string engine)
    : TableCatalogEntry(catalog, schema, info), clickhouse_columns(std::move(columns)), approx_rows(approx_rows),
      engine(std::move(engine)) {
}

unique_ptr<BaseStatistics> ClickhouseTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction ClickhouseTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<ClickhouseScanBindData>();
	auto &clickhouse_catalog = catalog.Cast<ClickhouseCatalog>();
	result->pool = clickhouse_catalog.GetConnectionPoolPtr();
	result->table_entry = this;
	// the bound plan may outlive the catalog's own reference to this entry (clickhouse_clear_cache()); keep
	// the schema that transitively owns it alive for as long as the plan holds table_entry. Null only if
	// another connection cleared the cache between the catalog lookup that found this entry and this call,
	// a window in which `this` is being freed underneath us either way.
	result->lifetime = clickhouse_catalog.GetSchemaEntryOwner(schema.name);
	result->database = schema.name;
	result->table = name;
	result->columns = clickhouse_columns;
	result->approx_rows = approx_rows;
	result->filter_pushdown = ClickhouseScanFunction::FilterPushdownEnabled(context);
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
