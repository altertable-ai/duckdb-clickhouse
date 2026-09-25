#include "storage/clickhouse_optimizer.hpp"

#include "clickhouse_filter_pushdown.hpp"
#include "clickhouse_scanner.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"

namespace duckdb {

//! The ClickHouse scan directly below op, looking through projections
static optional_ptr<LogicalGet> FindClickhouseScan(LogicalOperator &op) {
	reference<LogicalOperator> current = op;
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		current = *current.get().children[0];
	}
	if (current.get().type != LogicalOperatorType::LOGICAL_GET) {
		return nullptr;
	}
	auto &get = current.get().Cast<LogicalGet>();
	if (!ClickhouseScanFunction::IsClickhouseScan(get.function.name) || !get.bind_data) {
		return nullptr;
	}
	return &get;
}

//! A LIMIT may only move into ClickHouse when every table filter of the scan is on a pushable column and either
//! translates to a real ClickHouse predicate or is not required for correctness (TransformFilter returns "" for
//! those without throwing; see its header comment) -- a required filter DuckDB would still need to apply itself
//! must never be skipped by pushing the LIMIT past it.
//!
//! get.table_filters is keyed by the scan's own (absolute) column index, matching bind_data.columns directly --
//! see ClickhouseFilterPushdown::TransformFilters for the position-indexed variant used once a physical plan
//! exists.
static bool AllFiltersPushed(LogicalGet &get, const ClickhouseScanBindData &bind_data) {
	for (auto &entry : get.table_filters.filters) {
		auto column_id = entry.first;
		if (IsVirtualColumn(column_id) || column_id >= bind_data.columns.size() || !bind_data.filter_pushdown ||
		    !ClickhouseTypes::SupportsFilterPushdown(bind_data.columns[column_id].type_node)) {
			return false;
		}
		try {
			ClickhouseFilterPushdown::TransformFilter(
			    ClickhouseUtils::QuoteIdentifier(bind_data.columns[column_id].name), *entry.second);
		} catch (const NotImplementedException &) {
			// a required filter DuckDB cannot translate: ClickHouse would not enforce it, so a LIMIT applied
			// server-side could return the wrong rows
			return false;
		}
	}
	return true;
}

//! The quoted ClickHouse column that expr refers to (through projections), or "" if it is not a plain,
//! pushdown-safe column of the scan
static string TraceColumn(Expression &expr, LogicalOperator &child, LogicalGet &get,
                          const ClickhouseScanBindData &bind_data) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return string();
	}
	auto &column_ref = expr.Cast<BoundColumnRefExpression>();
	if (column_ref.depth > 0) {
		return string();
	}
	auto binding = column_ref.binding;
	reference<LogicalOperator> current = child;
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = current.get().Cast<LogicalProjection>();
		if (binding.table_index != projection.table_index || binding.column_index >= projection.expressions.size()) {
			return string();
		}
		auto &projected = *projection.expressions[binding.column_index];
		if (projected.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return string();
		}
		auto &inner = projected.Cast<BoundColumnRefExpression>();
		if (inner.depth > 0) {
			return string();
		}
		binding = inner.binding;
		current = *current.get().children[0];
	}
	if (binding.table_index != get.table_index) {
		return string();
	}
	auto &column_ids = get.GetColumnIds();
	if (binding.column_index >= column_ids.size() || column_ids[binding.column_index].IsVirtualColumn()) {
		return string();
	}
	auto column_id = column_ids[binding.column_index].GetPrimaryIndex();
	if (column_id >= bind_data.columns.size()) {
		return string();
	}
	auto &column = bind_data.columns[column_id];
	if (!ClickhouseTypes::SupportsPushdown(column.type_node)) {
		return string();
	}
	return ClickhouseUtils::QuoteIdentifier(column.name);
}

static string BuildOrderByClause(vector<BoundOrderByNode> &orders, LogicalOperator &child, LogicalGet &get,
                                 const ClickhouseScanBindData &bind_data) {
	vector<string> keys;
	for (auto &order : orders) {
		auto column = TraceColumn(*order.expression, child, get, bind_data);
		if (column.empty()) {
			return string();
		}
		// only push a direction/null-order we can render exactly; by the time the optimizer sees the plan these
		// should always already be resolved to one of the two concrete values, but don't guess if not
		if (order.type != OrderType::ASCENDING && order.type != OrderType::DESCENDING) {
			return string();
		}
		if (order.null_order != OrderByNullType::NULLS_FIRST && order.null_order != OrderByNullType::NULLS_LAST) {
			return string();
		}
		auto descending = order.type == OrderType::DESCENDING;
		auto nulls_first = order.null_order == OrderByNullType::NULLS_FIRST;
		keys.push_back(column + (descending ? " DESC" : " ASC") + (nulls_first ? " NULLS FIRST" : " NULLS LAST"));
	}
	return " ORDER BY " + StringUtil::Join(keys, ", ");
}

static void OptimizeRecursive(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_TOP_N) {
		auto &top_n = op->Cast<LogicalTopN>();
		auto get = FindClickhouseScan(*op->children[0]);
		if (get) {
			auto &bind_data = get->bind_data->Cast<ClickhouseScanBindData>();
			if (bind_data.limit_clause.empty() && AllFiltersPushed(*get, bind_data)) {
				auto order_by = BuildOrderByClause(top_n.orders, *op->children[0], *get, bind_data);
				if (!order_by.empty()) {
					bind_data.order_by_clause = order_by;
					bind_data.limit_clause = " LIMIT " + to_string(top_n.limit);
					if (top_n.offset > 0) {
						bind_data.limit_clause += " OFFSET " + to_string(top_n.offset);
					}
					op = std::move(op->children[0]);
					return;
				}
			}
		}
	} else if (op->type == LogicalOperatorType::LOGICAL_LIMIT) {
		auto &limit = op->Cast<LogicalLimit>();
		auto get = FindClickhouseScan(*op->children[0]);
		auto constant_limit = limit.limit_val.Type() == LimitNodeType::CONSTANT_VALUE;
		auto offset_type = limit.offset_val.Type();
		auto constant_offset = offset_type == LimitNodeType::CONSTANT_VALUE || offset_type == LimitNodeType::UNSET;
		if (get && constant_limit && constant_offset) {
			auto &bind_data = get->bind_data->Cast<ClickhouseScanBindData>();
			if (bind_data.limit_clause.empty() && AllFiltersPushed(*get, bind_data)) {
				bind_data.limit_clause = " LIMIT " + to_string(limit.limit_val.GetConstantValue());
				if (offset_type == LimitNodeType::CONSTANT_VALUE && limit.offset_val.GetConstantValue() > 0) {
					bind_data.limit_clause += " OFFSET " + to_string(limit.offset_val.GetConstantValue());
				}
				op = std::move(op->children[0]);
				return;
			}
		}
	}
	for (auto &child : op->children) {
		OptimizeRecursive(child);
	}
}

void ClickhouseOptimizer::Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	Value enabled;
	if (input.context.TryGetCurrentSetting("ch_order_pushdown", enabled) && !enabled.IsNull() &&
	    !BooleanValue::Get(enabled)) {
		return;
	}
	OptimizeRecursive(plan);
}

} // namespace duckdb
