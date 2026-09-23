#include "clickhouse_scanner.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "storage/clickhouse_catalog.hpp"

namespace duckdb {

static string StripTrailingSemicolons(string sql) {
	StringUtil::RTrim(sql);
	while (!sql.empty() && sql.back() == ';') {
		sql.pop_back();
		StringUtil::RTrim(sql);
	}
	return sql;
}

static unique_ptr<FunctionData> ClickhouseQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                     vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("Parameters to clickhouse_query cannot be NULL");
	}
	auto database_name = StringValue::Get(input.inputs[0]);
	auto database = DatabaseManager::Get(context).GetDatabase(context, database_name);
	if (!database) {
		throw BinderException("Failed to find attached database \"%s\" referenced in clickhouse_query",
		                       database_name);
	}
	auto &catalog = database->GetCatalog();
	if (catalog.GetCatalogType() != ClickhouseCatalog::CATALOG_TYPE) {
		throw BinderException("Attached database \"%s\" is not a ClickHouse database", database_name);
	}

	auto result = make_uniq<ClickhouseScanBindData>();
	result->pool = catalog.Cast<ClickhouseCatalog>().GetConnectionPoolPtr();
	result->query = StripTrailingSemicolons(StringValue::Get(input.inputs[1]));
	if (result->query.empty()) {
		throw BinderException("clickhouse_query: the query cannot be empty");
	}
	result->filter_pushdown = ClickhouseScanFunction::FilterPushdownEnabled(context);

	// DESCRIBE gives the result columns without running the query
	auto connection = result->pool->GetConnection();
	unordered_set<string> seen_names;
	for (auto &block : connection->Query("DESCRIBE TABLE (" + result->query + ")")) {
		auto column_names = block[0]->As<clickhouse::ColumnString>();
		auto column_types = block[1]->As<clickhouse::ColumnString>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			string name(column_names->At(row));
			if (!seen_names.insert(name).second) {
				throw BinderException("clickhouse_query: the query returns more than one column named \"%s\"; give "
				                       "its columns unique aliases",
				                       name);
			}
			result->columns.push_back(ClickhouseColumnInfo::Create(name, string(column_types->At(row))));
		}
	}
	if (result->columns.empty()) {
		throw BinderException("clickhouse_query: the query does not return any columns");
	}
	ClickhouseScanFunction::SetReturnTypes(*result, return_types, names);
	return std::move(result);
}

ClickhouseQueryFunction::ClickhouseQueryFunction()
    : TableFunction("clickhouse_query", {LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, ClickhouseQueryBind) {
	ClickhouseScanFunction::SetScanCallbacks(*this);
}

} // namespace duckdb
