#include "storage/clickhouse_schema_set.hpp"

#include "clickhouse_connection.hpp"
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
	auto &only_schema = ch_catalog.GetAttachOptions().schema;
	auto &default_database = ch_catalog.GetConfig().database;
	auto connection = ch_catalog.GetConnectionPool().GetConnection();
	auto blocks = connection->Query("SELECT name FROM system.databases ORDER BY name",
	                                ClickhouseConnection::ExtensionQuerySettings());
	for (auto &block : blocks) {
		auto names = block[0]->As<clickhouse::ColumnString>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			string name(names->At(row));
			if (!only_schema.empty()) {
				// SCHEMA wins over SHOW_SYSTEM and the connection's database
				if (name != only_schema) {
					continue;
				}
			} else if (IsSystemDatabase(name) && !show_system && name != default_database) {
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
