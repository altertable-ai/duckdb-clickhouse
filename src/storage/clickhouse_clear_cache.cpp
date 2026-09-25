#include "storage/clickhouse_clear_cache.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "storage/clickhouse_catalog.hpp"

namespace duckdb {

struct ClickhouseClearCacheData : public TableFunctionData {
	bool finished = false;
};

static unique_ptr<FunctionData> ClearCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return make_uniq<ClickhouseClearCacheData>();
}

void ClickhouseClearCacheFunction::ClearClickhouseCaches(ClientContext &context) {
	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &database : databases) {
		auto &catalog = database->GetCatalog();
		if (catalog.GetCatalogType() != ClickhouseCatalog::CATALOG_TYPE) {
			continue;
		}
		catalog.Cast<ClickhouseCatalog>().ClearCache();
	}
}

static void ClearCacheFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<ClickhouseClearCacheData>();
	if (data.finished) {
		return;
	}
	ClickhouseClearCacheFunction::ClearClickhouseCaches(context);
	data.finished = true;
}

ClickhouseClearCacheFunction::ClickhouseClearCacheFunction()
    : TableFunction("clickhouse_clear_cache", {}, ClearCacheFunction, ClearCacheBind) {
}

} // namespace duckdb
