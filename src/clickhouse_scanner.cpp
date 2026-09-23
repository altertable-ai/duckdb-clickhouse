#include "clickhouse_scanner.hpp"

#include "clickhouse_conversion.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

#include <optional>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//
unique_ptr<FunctionData> ClickhouseScanBindData::Copy() const {
	auto result = make_uniq<ClickhouseScanBindData>();
	result->pool = pool;
	result->table_entry = table_entry;
	result->database = database;
	result->table = table;
	result->query = query;
	result->columns = columns;
	result->approx_rows = approx_rows;
	result->filter_pushdown = filter_pushdown;
	result->order_by_clause = order_by_clause;
	result->limit_clause = limit_clause;
	return std::move(result);
}

bool ClickhouseScanBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<ClickhouseScanBindData>();
	return pool == other.pool && table_entry == other.table_entry && database == other.database &&
	       table == other.table && query == other.query && filter_pushdown == other.filter_pushdown &&
	       order_by_clause == other.order_by_clause && limit_clause == other.limit_clause;
}

//===--------------------------------------------------------------------===//
// State
//===--------------------------------------------------------------------===//
//! One ClickHouse query per scan. Workers take turns pulling blocks from it (under `lock`) and convert them in
//! parallel.
struct ClickhouseScanGlobalState : public GlobalTableFunctionState {
	~ClickhouseScanGlobalState() override {
		// the scan stopped early (LIMIT reached, error, interrupt): stop the server-side query
		if (connection && connection->IsQueryRunning()) {
			connection->Cancel();
		}
	}

	idx_t MaxThreads() const override {
		return max_threads;
	}

	mutex lock;
	ClickhousePoolConnection connection;
	string sql;
	vector<string> column_names;
	bool finished = false;
	idx_t next_batch_index = 0;
	idx_t max_threads = 1;
};

struct ClickhouseScanLocalState : public LocalTableFunctionState {
	std::optional<clickhouse::Block> block;
	idx_t offset = 0;
	idx_t batch_index = 0;
};

//===--------------------------------------------------------------------===//
// Query generation
//===--------------------------------------------------------------------===//
string ClickhouseScanFunction::BuildQuery(const ClickhouseScanBindData &bind_data,
                                           const vector<column_t> &column_ids,
                                           optional_ptr<TableFilterSet> filters) {
	vector<string> select_list;
	for (auto column_id : column_ids) {
		if (IsVirtualColumn(column_id)) {
			// row id / empty projection (e.g. count(*)): any value works, only the row count matters
			select_list.push_back("NULL");
			continue;
		}
		auto &column = bind_data.columns[column_id];
		if (!column.readable) {
			throw NotImplementedException(
			    "Column \"%s\" has ClickHouse type %s, which cannot be read by DuckDB. Use "
			    "clickhouse_query() with finalizeAggregation(%s) instead",
			    column.name, column.clickhouse_type, column.name);
		}
		select_list.push_back(
		    ClickhouseTypes::ReadExpression(column.type_node, ClickhouseUtils::QuoteIdentifier(column.name)));
	}
	if (select_list.empty()) {
		select_list.push_back("NULL");
	}
	string source;
	if (bind_data.query.empty()) {
		source = ClickhouseUtils::QuoteIdentifier(bind_data.database) + "." +
		         ClickhouseUtils::QuoteIdentifier(bind_data.table);
	} else {
		source = "(" + bind_data.query + ")";
	}
	auto sql = "SELECT " + StringUtil::Join(select_list, ", ") + " FROM " + source;
	sql += bind_data.order_by_clause;
	sql += bind_data.limit_clause;
	return sql;
}

void ClickhouseScanFunction::SetReturnTypes(const ClickhouseScanBindData &bind_data,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	for (auto &column : bind_data.columns) {
		names.push_back(column.name);
		return_types.push_back(column.type);
	}
}

bool ClickhouseScanFunction::IsClickhouseScan(const string &function_name) {
	return function_name == "clickhouse_scan" || function_name == "clickhouse_query";
}

//===--------------------------------------------------------------------===//
// Callbacks
//===--------------------------------------------------------------------===//
static unique_ptr<GlobalTableFunctionState> ClickhouseInitGlobal(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ClickhouseScanBindData>();
	auto result = make_uniq<ClickhouseScanGlobalState>();
	result->sql = ClickhouseScanFunction::BuildQuery(bind_data, input.column_ids, input.filters);
	for (auto column_id : input.column_ids) {
		result->column_names.push_back(IsVirtualColumn(column_id) ? "rowid" : bind_data.columns[column_id].name);
	}
	// a pushed-down ORDER BY must reach DuckDB in order: read with a single thread
	result->max_threads =
	    bind_data.order_by_clause.empty() ? MaxValue<idx_t>(1, context.db->NumberOfThreads()) : idx_t(1);
	result->connection = bind_data.pool->GetConnection();
	result->connection->BeginQuery(result->sql);
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> ClickhouseInitLocal(ExecutionContext &context,
                                                                TableFunctionInitInput &input,
                                                                GlobalTableFunctionState *global_state) {
	return make_uniq<ClickhouseScanLocalState>();
}

//! Takes the next non-empty block of the query for this worker; false when the query is exhausted
static bool FetchNextBlock(ClickhouseScanGlobalState &gstate, ClickhouseScanLocalState &lstate) {
	lock_guard<mutex> guard(gstate.lock);
	lstate.block.reset();
	lstate.offset = 0;
	while (!gstate.finished) {
		std::optional<clickhouse::Block> block;
		try {
			block = gstate.connection->NextBlock();
		} catch (...) {
			// the query failed: stop every other worker from touching this connection again (it is no
			// longer selecting, so a second NextBlock() on it would raise a spurious, unrelated error and
			// mark an otherwise-healthy connection broken) and release it before propagating the real error
			gstate.finished = true;
			gstate.connection = ClickhousePoolConnection();
			throw;
		}
		if (!block) {
			gstate.finished = true;
			// hand the connection back to the pool as soon as the query is done
			gstate.connection = ClickhousePoolConnection();
			break;
		}
		if (block->GetRowCount() == 0) {
			continue;
		}
		lstate.block = std::move(block);
		lstate.batch_index = gstate.next_batch_index++;
		return true;
	}
	return false;
}

static void ClickhouseScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<ClickhouseScanGlobalState>();
	auto &lstate = data.local_state->Cast<ClickhouseScanLocalState>();
	while (true) {
		if (lstate.block && lstate.offset < lstate.block->GetRowCount()) {
			auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, lstate.block->GetRowCount() - lstate.offset);
			ClickhouseConversion::ConvertBlock(*lstate.block, output, lstate.offset, count, gstate.column_names);
			lstate.offset += count;
			return;
		}
		if (!FetchNextBlock(gstate, lstate)) {
			output.SetCardinality(0);
			return;
		}
	}
}

static OperatorPartitionData ClickhouseGetPartitionData(ClientContext &context,
                                                          TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("ClickhouseScan::GetPartitionData: partition columns are not supported");
	}
	auto &lstate = input.local_state->Cast<ClickhouseScanLocalState>();
	return OperatorPartitionData(lstate.batch_index);
}

static unique_ptr<NodeStatistics> ClickhouseScanCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<ClickhouseScanBindData>();
	if (!bind_data.approx_rows.IsValid()) {
		return nullptr;
	}
	return make_uniq<NodeStatistics>(bind_data.approx_rows.GetIndex());
}

static InsertionOrderPreservingMap<string> ClickhouseScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<ClickhouseScanBindData>();
	if (bind_data.query.empty()) {
		result["Table"] = bind_data.database + "." + bind_data.table;
	} else {
		result["Query"] = bind_data.query;
	}
	auto pushed = bind_data.order_by_clause + bind_data.limit_clause;
	StringUtil::Trim(pushed);
	if (!pushed.empty()) {
		result["Pushed Down"] = pushed;
	}
	return result;
}

static InsertionOrderPreservingMap<string> ClickhouseScanDynamicToString(TableFunctionDynamicToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	if (input.global_state) {
		result["ClickHouse Query"] = input.global_state->Cast<ClickhouseScanGlobalState>().sql;
	}
	return result;
}

//! Without this, LogicalGet::GetTable() always returns null (it requires get_bind_info), which makes
//! UPDATE/DELETE's binder reject the scan before it ever reaches ClickhouseCatalog::PlanUpdate/PlanDelete.
static BindInfo ClickhouseGetBindInfo(const optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<ClickhouseScanBindData>();
	if (bind_data.table_entry) {
		return BindInfo(*bind_data.table_entry);
	}
	// clickhouse_scan() / clickhouse_query() (Task 9): no backing catalog entry
	return BindInfo(ScanType::EXTERNAL);
}

void ClickhouseScanFunction::SetScanCallbacks(TableFunction &function) {
	function.init_global = ClickhouseInitGlobal;
	function.init_local = ClickhouseInitLocal;
	function.function = ClickhouseScan;
	function.get_partition_data = ClickhouseGetPartitionData;
	function.cardinality = ClickhouseScanCardinality;
	function.to_string = ClickhouseScanToString;
	function.dynamic_to_string = ClickhouseScanDynamicToString;
	function.get_bind_info = ClickhouseGetBindInfo;
	function.projection_pushdown = true;
	function.filter_pushdown = false;
}

ClickhouseScanFunction::ClickhouseScanFunction()
    : TableFunction("clickhouse_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                    ClickhouseScan) {
	SetScanCallbacks(*this);
}

} // namespace duckdb
