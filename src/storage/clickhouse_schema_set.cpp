#include "storage/clickhouse_schema_set.hpp"

#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_schema_entry.hpp"

namespace duckdb {

ClickhouseSchemaSet::ClickhouseSchemaSet(Catalog &catalog) : ClickhouseCatalogSet(catalog) {
}

static bool IsSystemDatabase(const string &name) {
	return name == "system" || name == "INFORMATION_SCHEMA" || name == "information_schema";
}

void ClickhouseSchemaSet::LoadEntries(ClientContext &context) {
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	auto show_system = ch_catalog.GetAttachOptions().show_system;
	auto &default_database = ch_catalog.GetConfig().database;
	auto connection = ch_catalog.GetConnectionPool().GetConnection();
	auto blocks = connection->Query("SELECT name FROM system.databases ORDER BY name");
	for (auto &block : blocks) {
		auto names = block[0]->As<clickhouse::ColumnString>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			string name(names->At(row));
			if (IsSystemDatabase(name) && !show_system && name != default_database) {
				continue;
			}
			CreateSchemaInfo info;
			info.schema = name;
			info.internal = false;
			CreateEntry(make_uniq<ClickhouseSchemaEntry>(catalog, info));
		}
	}
}

} // namespace duckdb
