#include "storage/clickhouse_dml.hpp"

#include "clickhouse_expression.hpp"
#include "clickhouse_scanner.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_table_entry.hpp"

#include <clickhouse/columns/numeric.h>

namespace duckdb {

void ClickhouseDml::ThrowUnsupportedShape(const string &statement, const string &reason) {
	throw NotImplementedException("%s on ClickHouse tables must filter only the modified table with translatable "
	                              "expressions (%s); use clickhouse_execute() for anything else",
	                              statement, reason);
}

static const ClickhouseScanBindData &ScanBindData(const LogicalGet &get) {
	return get.bind_data->Cast<ClickhouseScanBindData>();
}

//! ClickHouse SQL for table column `column_id` of the scan: the same expression the scan reads it through (see
//! ClickhouseScanFunction::BuildQuery), so a predicate sees the values DuckDB sees. The bare quoted column for
//! every natively read type
static string ColumnSql(const ClickhouseScanBindData &bind_data, column_t column_id) {
	if (IsVirtualColumn(column_id) || column_id >= bind_data.columns.size()) {
		// rowid (always NULL for ClickHouse tables) or another virtual column
		throw NotImplementedException("reference to a virtual column");
	}
	auto &column = bind_data.columns[column_id];
	if (!column.readable) {
		throw NotImplementedException("column \"%s\" of type %s cannot be read", column.name, column.clickhouse_type);
	}
	if (!ClickhouseTypes::IsComparedExactly(column.type_node)) {
		// see ClickhouseTypes::IsComparedExactly: DuckDB and ClickHouse would disagree on which rows match
		throw NotImplementedException("column \"%s\" (%s) is not compared exactly", column.name,
		                              column.clickhouse_type);
	}
	return ClickhouseTypes::ReadExpression(column.type_node, ClickhouseUtils::QuoteIdentifier(column.name));
}

static constexpr const char *IN_LIST_JOIN = "IN list rewritten into a join";

//! Whether `op` looks like InClauseRewriter's MARK join (a MARK join over a LogicalColumnDataGet): such a join is
//! either exactly that shape (ValidateInListJoin) or rejected as an IN list. Any other join is some other operator
static bool IsInListJoinCandidate(const LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN || op.children.size() != 2) {
		return false;
	}
	return op.Cast<LogicalComparisonJoin>().join_type == JoinType::MARK &&
	       op.children[1]->type == LogicalOperatorType::LOGICAL_CHUNK_GET;
}

//! The exact shape InClauseRewriter::VisitReplace (duckdb/src/optimizer/in_clause_rewriter.cpp) builds for
//! `x [NOT] IN (<constants>)`: MARK join, the input on the left, a one-column LogicalColumnDataGet of the constants
//! on the right, one condition `x = <constant column>` and nothing else. Throws "IN list rewritten into a join"
//! for anything else
static const LogicalComparisonJoin &ValidateInListJoin(const LogicalOperator &op) {
	if (!IsInListJoinCandidate(op)) {
		throw NotImplementedException(IN_LIST_JOIN);
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	auto &constants = join.children[1]->Cast<LogicalColumnDataGet>();
	if (join.conditions.size() != 1 || join.predicate || !join.duplicate_eliminated_columns.empty() ||
	    !join.mark_types.empty() || join.delim_flipped || constants.chunk_types.size() != 1 || !constants.collection) {
		throw NotImplementedException(IN_LIST_JOIN);
	}
	auto &condition = join.conditions[0];
	if (condition.comparison != ExpressionType::COMPARE_EQUAL || !condition.left || !condition.right ||
	    condition.right->GetExpressionClass() != ExpressionClass::BOUND_REF ||
	    condition.right->Cast<BoundReferenceExpression>().index != 0) {
		throw NotImplementedException(IN_LIST_JOIN);
	}
	return join;
}

//! Number of output columns a MARK join passes through from its left input; the mark column comes right after them
static idx_t MarkJoinLeftCount(const LogicalComparisonJoin &join) {
	return join.left_projection_map.empty() ? join.children[0]->types.size() : join.left_projection_map.size();
}

//! The IN-list MARK join whose mark column output column `index` of `op` is, passed through filters and projections
//! as a plain reference; null if it is anything else
static optional_ptr<const LogicalComparisonJoin> FindMarkJoin(const LogicalOperator &op, idx_t index) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		if (!filter.projection_map.empty()) {
			if (index >= filter.projection_map.size()) {
				return nullptr;
			}
			index = filter.projection_map[index];
		}
		return FindMarkJoin(*op.children[0], index);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		if (index >= op.expressions.size() ||
		    op.expressions[index]->GetExpressionClass() != ExpressionClass::BOUND_REF) {
			return nullptr;
		}
		return FindMarkJoin(*op.children[0], op.expressions[index]->Cast<BoundReferenceExpression>().index);
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		if (!IsInListJoinCandidate(op)) {
			return nullptr;
		}
		auto &join = ValidateInListJoin(op);
		auto left_count = MarkJoinLeftCount(join);
		if (index == left_count) {
			return &join;
		}
		if (index > left_count) {
			return nullptr;
		}
		auto left_index = join.left_projection_map.empty() ? index : join.left_projection_map[index];
		return FindMarkJoin(*op.children[0], left_index);
	}
	default:
		return nullptr;
	}
}

//! A LogicalFilter expression over `input` that is exactly an IN-list mark column (IN) or NOT of one (NOT IN), as
//! `<x> [NOT] IN (<constants>)`; "" for any other expression. The mark is NULL when x is NULL (no NULL constants,
//! which are rejected), as is ClickHouse's x [NOT] IN (...) with the default transform_null_in = 0
static string TranslateInListFilter(const Expression &expr, const LogicalOperator &input) {
	auto inner = &expr;
	bool negated = false;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	    expr.GetExpressionType() == ExpressionType::OPERATOR_NOT) {
		auto &op = expr.Cast<BoundOperatorExpression>();
		if (op.children.size() != 1) {
			return string();
		}
		inner = op.children[0].get();
		negated = true;
	}
	if (inner->GetExpressionClass() != ExpressionClass::BOUND_REF) {
		return string();
	}
	auto join = FindMarkJoin(input, inner->Cast<BoundReferenceExpression>().index);
	if (!join) {
		return string();
	}
	auto &left_input = *join->children[0];
	auto left = ClickhouseExpression::Translate(*join->conditions[0].left,
	                                            [&](idx_t i) { return ClickhouseDml::ResolveOutput(left_input, i); });
	auto &constants = join->children[1]->Cast<LogicalColumnDataGet>();
	vector<string> values;
	for (auto &chunk : constants.collection->Chunks()) {
		for (idx_t row = 0; row < chunk.size(); row++) {
			auto value = chunk.GetValue(0, row);
			if (value.IsNull()) {
				// DuckDB's mark is NULL instead of false where ClickHouse's IN ignores the NULL: NOT IN differs
				throw NotImplementedException("IN list holding NULL");
			}
			values.push_back(ClickhouseExpression::Literal(value));
		}
	}
	if (values.empty()) {
		throw NotImplementedException(IN_LIST_JOIN);
	}
	return "(" + left + (negated ? " NOT IN (" : " IN (") + StringUtil::Join(values, ", ") + "))";
}

//! Whether any expression in the plan is (or holds) a prepared-statement parameter
static bool HasParameters(LogicalOperator &op) {
	bool found = false;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](unique_ptr<Expression> *expr) {
		if (*expr && (*expr)->HasParameter()) {
			found = true;
		}
	});
	for (auto &child : op.children) {
		found = found || HasParameters(*child);
	}
	return found;
}

string ClickhouseDml::ResolveOutput(const LogicalOperator &op, idx_t index) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		auto &column_ids = get.GetColumnIds();
		idx_t position = index;
		if (!get.projection_ids.empty()) {
			if (index >= get.projection_ids.size()) {
				throw NotImplementedException("reference outside the scan's output");
			}
			position = get.projection_ids[index];
		}
		if (position >= column_ids.size()) {
			throw NotImplementedException("reference outside the scan's output");
		}
		auto &column_index = column_ids[position];
		if (column_index.HasChildren()) {
			throw NotImplementedException("reference to a nested field pushed into the scan");
		}
		return ColumnSql(ScanBindData(get), column_index.GetPrimaryIndex());
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		idx_t child_index = index;
		if (!filter.projection_map.empty()) {
			if (index >= filter.projection_map.size()) {
				throw NotImplementedException("reference outside the filter's output");
			}
			child_index = filter.projection_map[index];
		}
		return ResolveOutput(*op.children[0], child_index);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		if (index >= op.expressions.size()) {
			throw NotImplementedException("reference outside the projection's output");
		}
		auto &child = *op.children[0];
		return "(" +
		       ClickhouseExpression::Translate(*op.expressions[index],
		                                       [&](idx_t i) { return ResolveOutput(child, i); }) +
		       ")";
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		if (!IsInListJoinCandidate(op)) {
			throw NotImplementedException("operator %s", LogicalOperatorToString(op.type));
		}
		auto &join = ValidateInListJoin(op);
		auto left_count = MarkJoinLeftCount(join);
		if (index >= left_count) {
			// the mark column anywhere but as a whole filter expression (or under a NOT), e.g. x IN (...) OR y
			throw NotImplementedException(IN_LIST_JOIN);
		}
		auto left_index = join.left_projection_map.empty() ? index : join.left_projection_map[index];
		return ResolveOutput(*op.children[0], left_index);
	}
	default:
		throw NotImplementedException("operator %s", LogicalOperatorToString(op.type));
	}
}

ClickhouseDmlTarget ClickhouseDml::AnalyzeTarget(const string &statement, TableCatalogEntry &table,
                                                 LogicalOperator &child) {
	auto &ch_table = table.Cast<ClickhouseTableEntry>();
	if (ch_table.IsViewLike()) {
		throw NotImplementedException("\"%s\" is a ClickHouse %s (engine %s), which %s does not modify; run the "
		                              "statement with clickhouse_execute() instead",
		                              ch_table.name, ch_table.ViewLikeKind(), ch_table.GetEngine(), statement);
	}
	ClickhouseDmlTarget result {ch_table, ClickhouseUtils::QualifiedName(ch_table.schema.name, ch_table.name), ""};
	if (HasParameters(child)) {
		// PREPARE: nothing to translate yet; EXECUTE plans the statement again with the values
		result.unbound_parameters = true;
		return result;
	}
	vector<string> conditions;
	try {
		vector<reference<LogicalFilter>> filters;
		reference<LogicalOperator> current = child;
		while (true) {
			auto &op = current.get();
			if (op.type == LogicalOperatorType::LOGICAL_PROJECTION || op.type == LogicalOperatorType::LOGICAL_FILTER) {
				if (op.children.size() != 1) {
					throw NotImplementedException("operator with several inputs");
				}
				if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
					filters.push_back(op.Cast<LogicalFilter>());
				}
			} else if (IsInListJoinCandidate(op)) {
				// the constants side needs no translation of its own: TranslateInListFilter reads it
				ValidateInListJoin(op);
			} else {
				break;
			}
			current = *op.children[0];
		}
		if (current.get().type == LogicalOperatorType::LOGICAL_EMPTY_RESULT) {
			// DuckDB proved that no row matches (e.g. WHERE 1 = 0, WHERE NULL)
			result.matches_nothing = true;
			return result;
		}
		if (current.get().type != LogicalOperatorType::LOGICAL_GET) {
			throw NotImplementedException("operator %s", LogicalOperatorToString(current.get().type));
		}
		auto &get = current.get().Cast<LogicalGet>();
		if (!ClickhouseScanFunction::IsClickhouseScan(get.function.name) || !get.bind_data ||
		    ScanBindData(get).table_entry != &table) {
			throw NotImplementedException("reads another table");
		}
		if (!get.children.empty()) {
			throw NotImplementedException("scan with an input");
		}
		auto &bind_data = ScanBindData(get);
		// at the logical level (before PhysicalPlanGenerator::CreatePlan(LogicalGet) renumbers them for the
		// physical scan), table_filters is keyed by the table column id itself (TableFilterSet::PushFilter keys by
		// ColumnIndex::GetPrimaryIndex()), not by position in the get's column_ids. Every filter is here, whether
		// or not the scan would push it into ClickHouse for a SELECT (supports_pushdown_type is only consulted
		// when the get is planned)
		for (auto &entry : get.table_filters.filters) {
			auto column_id = entry.first;
			if (IsVirtualColumn(column_id) || column_id >= bind_data.columns.size()) {
				throw NotImplementedException("filter on a virtual column");
			}
			auto sql = ClickhouseExpression::TranslateTableFilter(*entry.second, bind_data.columns[column_id].type,
			                                                      ColumnSql(bind_data, column_id));
			if (!sql.empty()) {
				conditions.push_back(sql);
			}
		}
		for (auto &filter : filters) {
			auto &filter_child = *filter.get().children[0];
			for (auto &expr : filter.get().expressions) {
				auto in_list = TranslateInListFilter(*expr, filter_child);
				if (!in_list.empty()) {
					conditions.push_back(in_list);
					continue;
				}
				conditions.push_back(ClickhouseExpression::Translate(
				    *expr, [&](idx_t i) { return ResolveOutput(filter_child, i); }));
			}
		}
	} catch (NotImplementedException &ex) {
		ErrorData error(ex);
		ThrowUnsupportedShape(statement, error.RawMessage());
	}
	result.predicate = StringUtil::Join(conditions, " AND ");
	return result;
}

ClickhouseDmlStatement ClickhouseDml::PlanDelete(LogicalDelete &op) {
	if (op.return_chunk) {
		throw NotImplementedException("RETURNING is not supported for ClickHouse tables");
	}
	auto target = AnalyzeTarget("DELETE", op.table, *op.children[0]);
	ClickhouseDmlStatement result;
	result.catalog_name = op.table.catalog.GetName();
	if (target.unbound_parameters) {
		result.description = "DELETE FROM " + target.qualified_name;
		result.unbound_parameters = true;
		return result;
	}
	if (target.matches_nothing) {
		// no count, no statement, no connection
		result.description = "DELETE FROM " + target.qualified_name;
		return result;
	}
	result.count_sql = "SELECT count() FROM " + target.qualified_name;
	if (target.predicate.empty()) {
		result.description = "TRUNCATE TABLE " + target.qualified_name;
		result.sql = result.description;
	} else {
		result.count_sql += " WHERE " + target.predicate;
		result.description = "DELETE FROM " + target.qualified_name;
		result.sql = result.description + " WHERE " + target.predicate;
		// lightweight DELETE, synchronous (spec D3)
		result.settings.emplace_back("lightweight_deletes_sync", "2");
	}
	return result;
}

//! The table column id that output column `index` of `op` passes through unchanged from the ClickHouse scan
//! (plain references through projections, filters and an IN-list MARK join's input); invalid for anything else.
//! Structural, so it does not depend on the SQL ResolveOutput would produce for the column (a read expression, or
//! a rejection for a column that is not compared exactly)
static optional_idx UnchangedScanColumn(const LogicalOperator &op, idx_t index) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		auto &column_ids = get.GetColumnIds();
		idx_t position = index;
		if (!get.projection_ids.empty()) {
			if (index >= get.projection_ids.size()) {
				return optional_idx();
			}
			position = get.projection_ids[index];
		}
		if (position >= column_ids.size() || column_ids[position].HasChildren() ||
		    IsVirtualColumn(column_ids[position].GetPrimaryIndex())) {
			return optional_idx();
		}
		return column_ids[position].GetPrimaryIndex();
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		if (!filter.projection_map.empty()) {
			if (index >= filter.projection_map.size()) {
				return optional_idx();
			}
			index = filter.projection_map[index];
		}
		return UnchangedScanColumn(*op.children[0], index);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		if (index >= op.expressions.size() ||
		    op.expressions[index]->GetExpressionClass() != ExpressionClass::BOUND_REF) {
			return optional_idx();
		}
		return UnchangedScanColumn(*op.children[0], op.expressions[index]->Cast<BoundReferenceExpression>().index);
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		if (!IsInListJoinCandidate(op)) {
			return optional_idx();
		}
		auto &join = op.Cast<LogicalComparisonJoin>();
		auto left_count = MarkJoinLeftCount(join);
		if (index >= left_count) {
			return optional_idx();
		}
		auto left_index = join.left_projection_map.empty() ? index : join.left_projection_map[index];
		return UnchangedScanColumn(*op.children[0], left_index);
	}
	default:
		return optional_idx();
	}
}

ClickhouseDmlStatement ClickhouseDml::PlanUpdate(ClientContext &context, LogicalUpdate &op) {
	if (op.return_chunk) {
		throw NotImplementedException("RETURNING is not supported for ClickHouse tables");
	}
	auto target = AnalyzeTarget("UPDATE", op.table, *op.children[0]);
	// the DuckDB and ClickHouse column lists match one to one (GetScanFunction already refused a table where they
	// do not)
	target.table.ThrowIfColumnsCollide();
	auto &columns = target.table.GetClickhouseColumns();
	if (op.columns.size() != op.expressions.size()) {
		ThrowUnsupportedShape("UPDATE", "SET list does not match its columns");
	}
	for (idx_t i = 0; i < op.columns.size(); i++) {
		if (op.columns[i].index >= columns.size()) {
			ThrowUnsupportedShape("UPDATE", "SET of an unknown column");
		}
		if (op.expressions[i]->GetExpressionClass() == ExpressionClass::BOUND_DEFAULT) {
			// ClickHouse has no DEFAULT in ALTER TABLE … UPDATE; checked before the WHERE is even looked at
			ThrowUnsupportedShape("UPDATE", "SET " + columns[op.columns[i].index].name + " = DEFAULT");
		}
	}
	ClickhouseDmlStatement result;
	result.catalog_name = op.table.catalog.GetName();
	result.description = "ALTER TABLE " + target.qualified_name + " UPDATE";
	// both flags come with an empty predicate, which must never become WHERE 1 (every row)
	if (target.unbound_parameters) {
		result.unbound_parameters = true;
		return result;
	}
	if (target.matches_nothing) {
		// no count, no statement, no connection
		return result;
	}
	auto &child = *op.children[0];
	vector<string> assignments;
	try {
		for (idx_t i = 0; i < op.columns.size(); i++) {
			auto column_id = op.columns[i].index;
			auto &column = columns[column_id];
			auto &expr = *op.expressions[i];
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
				auto source = UnchangedScanColumn(child, expr.Cast<BoundReferenceExpression>().index);
				if (source.IsValid() && source.GetIndex() == column_id) {
					// col = col: DuckDB adds every column like this to some UPDATEs (e.g. of an array column, which
					// it turns into DELETE + INSERT); sending them would fail on key columns
					continue;
				}
			}
			auto value =
			    ClickhouseExpression::Translate(expr, [&](idx_t index) { return ResolveOutput(child, index); });
			assignments.push_back(ClickhouseUtils::QuoteIdentifier(column.name) + " = CAST(" + value + " AS " +
			                      column.clickhouse_type + ")");
		}
	} catch (NotImplementedException &ex) {
		ErrorData error(ex);
		ThrowUnsupportedShape("UPDATE", error.RawMessage());
	}
	auto where = target.predicate.empty() ? string("1") : target.predicate;
	result.count_sql = "SELECT count() FROM " + target.qualified_name + " WHERE " + where;
	if (assignments.empty()) {
		// nothing changes: count only
		return result;
	}
	result.sql = result.description + " " + StringUtil::Join(assignments, ", ") + " WHERE " + where;
	Value mutations_sync;
	string sync = "2";
	if (context.TryGetCurrentSetting("ch_mutations_sync", mutations_sync) && !mutations_sync.IsNull()) {
		sync = mutations_sync.ToString();
	}
	result.settings.emplace_back("mutations_sync", sync);
	return result;
}

ClickhouseDmlOperator::ClickhouseDmlOperator(PhysicalPlan &physical_plan, LogicalOperator &op,
                                             ClickhouseDmlStatement statement_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1),
      statement(std::move(statement_p)) {
}

SourceResultType ClickhouseDmlOperator::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                        OperatorSourceInput &input) const {
	if (statement.unbound_parameters) {
		// never expected: DuckDB plans a prepared statement again, with its parameter values, on every EXECUTE
		throw InvalidInputException("%s was planned without its prepared-statement parameter values and cannot run; "
		                            "prepare the statement again",
		                            statement.description);
	}
	uint64_t count = 0;
	if (!statement.count_sql.empty()) {
		auto &catalog =
		    ClickhouseCatalog::GetAttachedDatabase(context.client, statement.catalog_name, statement.description);
		auto connection = catalog.StartWrite(context.client);
		for (auto &block : connection->Query(statement.count_sql)) {
			if (block.GetRowCount() == 0) {
				continue;
			}
			auto column = block.GetColumnCount() == 1 ? block[0]->As<clickhouse::ColumnUInt64>() : nullptr;
			if (!column) {
				throw InvalidInputException("%s: unexpected result for the affected-row count (%s)",
				                            statement.description, statement.count_sql);
			}
			count = column->At(0);
		}
		if (!statement.sql.empty()) {
			connection->Execute(statement.sql, statement.settings);
		}
	}
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(count)));
	return SourceResultType::FINISHED;
}

string ClickhouseDmlOperator::GetName() const {
	return "CLICKHOUSE_DML";
}

InsertionOrderPreservingMap<string> ClickhouseDmlOperator::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	if (statement.unbound_parameters) {
		result["Statement"] = statement.description + " (planned again with the parameter values on EXECUTE)";
	} else if (statement.count_sql.empty()) {
		result["Statement"] = statement.description + " (nothing to run: no row matches)";
	} else if (statement.sql.empty()) {
		result["Statement"] = statement.count_sql + " (count only: nothing changes)";
	} else {
		result["Statement"] = statement.sql;
	}
	return result;
}

} // namespace duckdb
