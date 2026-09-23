#include "clickhouse_scanner.hpp"

#include "clickhouse_conversion.hpp"
#include "clickhouse_filter_pushdown.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
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
	//! Which of column_names / the fetched block's columns must reach `output`, and in what order; empty means
	//! all of them, in order (see ClickhouseInitGlobal)
	vector<idx_t> projection_ids;
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
	auto where_clause = ClickhouseFilterPushdown::TransformFilters(column_ids, filters, bind_data.columns);
	if (!where_clause.empty()) {
		sql += " WHERE " + where_clause;
	}
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
	// columns needed only to evaluate a filter DuckDB couldn't push down (ClickhouseSupportsPushdownType
	// returned false for them) are still fetched -- filter_prune=true lets DuckDB tell us, via
	// projection_ids, which of the fetched columns actually need to reach `output`
	result->projection_ids = input.projection_ids;
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
			ClickhouseConversion::ConvertBlock(*lstate.block, output, lstate.offset, count, gstate.column_names,
			                                  gstate.projection_ids);
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

//! DuckDB only hands us filters on columns for which this returns true; it evaluates all others itself
static bool ClickhouseSupportsPushdownType(const FunctionData &bind_data_p, idx_t column_index) {
	auto &bind_data = bind_data_p.Cast<ClickhouseScanBindData>();
	if (!bind_data.filter_pushdown || column_index >= bind_data.columns.size()) {
		return false;
	}
	return ClickhouseTypes::SupportsPushdown(bind_data.columns[column_index].type_node);
}

bool ClickhouseScanFunction::FilterPushdownEnabled(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting("ch_filter_pushdown", value) && !value.IsNull()) {
		return BooleanValue::Get(value);
	}
	return true;
}

//! `CAST(<column> AS VARCHAR)`, unwrapped to the column reference inside, or null when expr isn't that shape
static const BoundColumnRefExpression *AsVarcharCastOfColumnRef(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return nullptr;
	}
	auto &cast_expr = expr.Cast<BoundCastExpression>();
	if (cast_expr.return_type.id() != LogicalTypeId::VARCHAR ||
	    cast_expr.child->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	return &cast_expr.child->Cast<BoundColumnRefExpression>();
}

//! Resolves a BoundColumnRefExpression to its ClickHouse column, when that column may have filters pushed down
static optional_idx ResolvePushdownableColumn(const LogicalGet &get, const ClickhouseScanBindData &bind_data,
                                              const BoundColumnRefExpression &column_ref) {
	if (!bind_data.filter_pushdown) {
		return optional_idx();
	}
	auto &get_column_ids = get.GetColumnIds();
	if (column_ref.binding.column_index >= get_column_ids.size()) {
		return optional_idx();
	}
	auto column_id = get_column_ids[column_ref.binding.column_index].GetPrimaryIndex();
	if (IsVirtualColumn(column_id) || column_id >= bind_data.columns.size()) {
		return optional_idx();
	}
	if (!ClickhouseTypes::SupportsPushdown(bind_data.columns[column_id].type_node)) {
		return optional_idx();
	}
	return optional_idx(column_id);
}

//! DuckDB's filter combiner (duckdb/src/optimizer/filter_combiner.cpp) never turns a bare `col IS [NOT] NULL`
//! into a TableFilterType::IS_NULL / IS_NOT_NULL constant filter for a generic table function the way it does
//! for `=`, `<`, IN, etc. -- that filter stays a plain LogicalFilter above the scan unless the table function
//! opts into generic-expression pushdown here, which hands back an ExpressionFilter instead. Same story for
//! `enum_col = 'literal'`: DuckDB's binder rewrites it to `CAST(enum_col AS VARCHAR) = 'literal'` (casting a
//! VARCHAR literal to ENUM isn't implicit), so it never becomes a plain constant-comparison filter either.
//! Only recognize the two shapes ClickhouseFilterPushdown::TransformFilter can translate exactly.
static bool ClickhousePushdownExpression(ClientContext &context, const LogicalGet &get, Expression &expr) {
	if (!get.bind_data) {
		return false;
	}
	auto &bind_data = get.bind_data->Cast<ClickhouseScanBindData>();

	if (expr.type == ExpressionType::OPERATOR_IS_NULL || expr.type == ExpressionType::OPERATOR_IS_NOT_NULL) {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		if (op_expr.children.size() != 1 ||
		    op_expr.children[0]->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return false;
		}
		auto &column_ref = op_expr.children[0]->Cast<BoundColumnRefExpression>();
		return ResolvePushdownableColumn(get, bind_data, column_ref).IsValid();
	}

	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON &&
	    (expr.type == ExpressionType::COMPARE_EQUAL || expr.type == ExpressionType::COMPARE_NOTEQUAL)) {
		auto &comparison = expr.Cast<BoundComparisonExpression>();
		auto *column_ref = AsVarcharCastOfColumnRef(*comparison.left);
		auto *other = comparison.right.get();
		if (!column_ref) {
			column_ref = AsVarcharCastOfColumnRef(*comparison.right);
			other = comparison.left.get();
		}
		if (!column_ref || other->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
			return false;
		}
		auto column_id = ResolvePushdownableColumn(get, bind_data, *column_ref);
		// only ENUM columns get the CAST-to-VARCHAR treatment from the binder; comparing the string form of
		// any other type (dates, decimals, ...) is not guaranteed to match ClickHouse's own formatting
		return column_id.IsValid() && bind_data.columns[column_id.GetIndex()].type.id() == LogicalTypeId::ENUM;
	}

	return false;
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
	function.filter_pushdown = true;
	// lets DuckDB immediately drop columns that are only needed to evaluate a filter it couldn't push down
	// (see ClickhouseSupportsPushdownType / the projection_ids handling in ClickhouseInitGlobal); required
	// for correctness once supports_pushdown_type is set, not just an optimization -- see
	// duckdb/src/optimizer/remove_unused_columns.cpp (only populates LogicalGet::projection_ids when this is
	// true) and duckdb/src/execution/physical_plan/plan_get.cpp (always sizes the scan's output off it once a
	// filter on an unsupported column exists)
	function.filter_prune = true;
	function.supports_pushdown_type = ClickhouseSupportsPushdownType;
	function.pushdown_expression = ClickhousePushdownExpression;
}

ClickhouseScanFunction::ClickhouseScanFunction()
    : TableFunction("clickhouse_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                    ClickhouseScan) {
	SetScanCallbacks(*this);
}

} // namespace duckdb
