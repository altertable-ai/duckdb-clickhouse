#include "storage/clickhouse_table_entry.hpp"

#include "clickhouse_scanner.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/storage/table_storage_info.hpp"
#include "storage/clickhouse_catalog.hpp"

namespace duckdb {

ClickhouseTableEntry::ClickhouseTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                           vector<ClickhouseColumnInfo> columns, optional_idx approx_rows,
                                           string engine, string column_collision)
    : TableCatalogEntry(catalog, schema, info), clickhouse_columns(std::move(columns)), approx_rows(approx_rows),
      engine(std::move(engine)), column_collision(std::move(column_collision)) {
}

void ClickhouseTableEntry::ThrowIfColumnsCollide() const {
	if (column_collision.empty()) {
		return;
	}
	throw InvalidInputException("ClickHouse table \"%s\".\"%s\" has columns whose names differ only in case (%s), "
	                            "which DuckDB cannot tell apart; rename one of them (ALTER TABLE … RENAME COLUMN), or "
	                            "read the table with clickhouse_query()",
	                            schema.name, name, column_collision);
}

unique_ptr<BaseStatistics> ClickhouseTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction ClickhouseTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	// the scan maps DuckDB column ids onto clickhouse_columns, which only line up without a collision
	ThrowIfColumnsCollide();
	auto result = make_uniq<ClickhouseScanBindData>();
	auto &clickhouse_catalog = catalog.Cast<ClickhouseCatalog>();
	result->pool = clickhouse_catalog.GetConnectionPoolPtr();
	result->table_entry = this;
	// the bound plan may outlive the catalog's own reference to this entry (clickhouse_clear_cache(), any DDL):
	// retirement keeps the entry alive until the binding transaction ends (see ClickhouseTransactionManager); keep
	// the schema that transitively owns it alive for as long as the plan holds table_entry, beyond that. Null if
	// another connection cleared the cache between the catalog lookup that found this entry and this call; the
	// entry is then retired and stays valid for the rest of this transaction.
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
