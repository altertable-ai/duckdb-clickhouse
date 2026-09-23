#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/function/table_function.hpp"
#include "storage/clickhouse_connection_pool.hpp"

namespace duckdb {

struct ClickhouseScanBindData : public TableFunctionData {
	//! Pool of the attached catalog, or a private pool for clickhouse_scan() without ATTACH
	shared_ptr<ClickhouseConnectionPool> pool;
	//! Scanned table (attached tables and clickhouse_scan)
	string database;
	string table;
	//! Wrapped query (clickhouse_query); when set, database/table are unused
	string query;
	vector<ClickhouseColumnInfo> columns;
	optional_idx approx_rows;
	//! ch_filter_pushdown at bind time
	bool filter_pushdown = true;
	//! Filled by ClickhouseOptimizer, e.g. " ORDER BY `n` DESC NULLS LAST" and " LIMIT 3"
	string order_by_clause;
	string limit_clause;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

class ClickhouseScanFunction : public TableFunction {
public:
	ClickhouseScanFunction();

	//! Installs the scan callbacks shared by clickhouse_scan and clickhouse_query
	static void SetScanCallbacks(TableFunction &function);
	//! The ClickHouse query for the projected columns, pushed-down filters and ORDER BY / LIMIT
	static string BuildQuery(const ClickhouseScanBindData &bind_data, const vector<column_t> &column_ids,
	                          optional_ptr<TableFilterSet> filters);
	static void SetReturnTypes(const ClickhouseScanBindData &bind_data, vector<LogicalType> &return_types,
	                            vector<string> &names);
	static bool IsClickhouseScan(const string &function_name);
};

} // namespace duckdb
