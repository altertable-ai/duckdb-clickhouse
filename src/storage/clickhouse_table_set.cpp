#include "storage/clickhouse_table_set.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_table_entry.hpp"

namespace duckdb {

ClickhouseTableSet::ClickhouseTableSet(SchemaCatalogEntry &schema, Catalog &catalog)
    : ClickhouseCatalogSet(catalog), schema(schema) {
}

struct ClickhouseTableDefinition {
	string name;
	vector<ClickhouseColumnInfo> columns;
};

void ClickhouseTableSet::LoadEntries(ClientContext &context) {
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	auto connection = ch_catalog.GetConnectionPool().GetConnection();
	auto database = ClickhouseUtils::QuoteLiteral(schema.name);

	unordered_map<string, idx_t> row_counts;
	for (auto &block :
	     connection->Query("SELECT name, total_rows FROM system.tables WHERE database = " + database)) {
		auto names = block[0]->As<clickhouse::ColumnString>();
		auto totals = block[1]->As<clickhouse::ColumnNullable>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			if (!totals->IsNull(row)) {
				row_counts[string(names->At(row))] =
				    totals->Nested()->As<clickhouse::ColumnUInt64>()->At(row);
			}
		}
	}

	// EPHEMERAL columns only exist for INSERTs and cannot be selected
	auto columns_query = "SELECT table, name, type FROM system.columns WHERE database = " + database +
	                     " AND default_kind != 'EPHEMERAL' ORDER BY table, position";
	vector<ClickhouseTableDefinition> tables;
	for (auto &block : connection->Query(columns_query)) {
		auto table_names = block[0]->As<clickhouse::ColumnString>();
		auto column_names = block[1]->As<clickhouse::ColumnString>();
		auto column_types = block[2]->As<clickhouse::ColumnString>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			string table_name(table_names->At(row));
			if (tables.empty() || tables.back().name != table_name) {
				tables.push_back(ClickhouseTableDefinition {table_name, {}});
			}
			tables.back().columns.push_back(
			    ClickhouseColumnInfo::Create(string(column_names->At(row)), string(column_types->At(row))));
		}
	}

	for (auto &table : tables) {
		CreateTableInfo info(schema, table.name);
		for (idx_t i = 0; i < table.columns.size(); i++) {
			auto &column = table.columns[i];
			info.columns.AddColumn(ColumnDefinition(column.name, column.type));
			if (!ClickhouseTypes::IsNullable(column.type_node)) {
				info.constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(i)));
			}
		}
		optional_idx approx_rows;
		auto row_count = row_counts.find(table.name);
		if (row_count != row_counts.end()) {
			approx_rows = row_count->second;
		}
		CreateEntry(
		    make_uniq<ClickhouseTableEntry>(catalog, schema, info, std::move(table.columns), approx_rows));
	}
}

} // namespace duckdb
