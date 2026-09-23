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

static bool IsIdentifierCharacter(char c) {
	return StringUtil::CharacterIsAlphaNumeric(c) || c == '_';
}

//! The start of the identifier ending at `end`, or `end` itself when the character before it is not part of
//! one
static idx_t IdentifierStart(const string &sql, idx_t end) {
	auto start = end;
	while (start > 0 && IsIdentifierCharacter(sql[start - 1])) {
		start--;
	}
	return start;
}

//! Whether sql ends in a `FORMAT <identifier>` clause. ClickHouse accepts FORMAT only on the outermost
//! query, and every query given to clickhouse_query is wrapped (in `DESCRIBE TABLE (...)` here, in a
//! subquery in ClickhouseScanFunction::BuildQuery), so such a query cannot be run at all -- rejecting it
//! with an explanation beats the bare SYNTAX_ERROR the server would otherwise report. Only a bare trailing
//! identifier counts, so the word inside a string literal (`SELECT 'FORMAT JSON'`, which ends in a quote)
//! or a function call (`SELECT format('{}', 1)`, which ends in a paren) is not mistaken for a clause.
static bool HasTrailingFormatClause(const string &sql) {
	auto format_name_start = IdentifierStart(sql, sql.size());
	if (format_name_start == sql.size() || format_name_start == 0) {
		return false;
	}
	auto keyword_end = format_name_start;
	while (keyword_end > 0 && StringUtil::CharacterIsSpace(sql[keyword_end - 1])) {
		keyword_end--;
	}
	if (keyword_end == format_name_start) {
		return false;
	}
	auto keyword_start = IdentifierStart(sql, keyword_end);
	return StringUtil::CIEquals(sql.substr(keyword_start, keyword_end - keyword_start), "FORMAT");
}

//! Unlike clickhouse_scan(), this needs no enable_external_access check: it can only reach the connection
//! pool of an already attached ClickHouse catalog, which ClickhouseAttach only creates when the setting
//! allows it.
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
	if (HasTrailingFormatClause(result->query)) {
		throw BinderException("clickhouse_query: the query cannot end in a FORMAT clause -- it is wrapped in a "
		                       "subquery, where ClickHouse does not accept one. Remove the clause and let DuckDB "
		                       "format the result");
	}
	result->filter_pushdown = ClickhouseScanFunction::FilterPushdownEnabled(context);

	// DESCRIBE gives the result columns without running the query; the newlines keep a trailing line
	// comment in the query from commenting out the closing paren
	auto connection = result->pool->GetConnection();
	unordered_set<string> seen_names;
	for (auto &block : connection->Query("DESCRIBE TABLE (\n" + result->query + "\n)")) {
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
