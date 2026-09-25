#include "storage/clickhouse_dml.hpp"

#include "clickhouse_ddl_types.hpp"
#include "clickhouse_expression.hpp"
#include "clickhouse_scanner.hpp"
#include "clickhouse_utils.hpp"
#include "clickhouse_writer.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
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

void ClickhouseDml::ThrowUnsupportedShape(const string &statement, const TableCatalogEntry &table,
                                          const string &reason) {
	throw NotImplementedException("%s on ClickHouse table %s must filter only the modified table with translatable "
	                              "expressions (%s); use clickhouse_execute() for anything else",
	                              statement, DisplayName(table), reason);
}

string ClickhouseDml::DisplayName(const TableCatalogEntry &table) {
	return KeywordHelper::WriteQuoted(table.schema.name, '"') + "." + KeywordHelper::WriteQuoted(table.name, '"');
}

//! Whether TRUNCATE TABLE really removes the rows of a table with this engine: the MergeTree family (Replicated,
//! Shared, Summing, ... included), Memory, the Log family, Set and Join. Not, e.g., Distributed (whose TRUNCATE
//! leaves the shards' data alone), Merge, Buffer or the integration engines
static bool TruncateRemovesRows(const string &engine) {
	static const unordered_set<string> ENGINES = {"Memory", "Log", "TinyLog", "StripeLog", "Set", "Join"};
	return StringUtil::EndsWith(engine, "MergeTree") || ENGINES.find(engine) != ENGINES.end();
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

//! Where output column `index` of a plan operator comes from, one operator down (StepReference) or followed through
//! every plain reference (TraceReference). The one walk ResolveOutput, FindMarkJoin and UnchangedScanColumn share
struct ReferenceStep {
	enum class Kind : uint8_t {
		//! The same value as output `index` of `*next`: a filter, a projection of a plain reference, or the left input
		//! of an IN-list MARK join
		PASS_THROUGH,
		//! Table column `column_id` (possibly a virtual column) of the LogicalGet `*op`
		SCAN_COLUMN,
		//! Computed by `op->expressions[index]` of the LogicalProjection `*op`, which is not a plain reference
		PROJECTION_EXPRESSION,
		//! The mark column of the IN-list MARK join `*op` (validated)
		MARK_COLUMN,
		//! Anything else, e.g. another operator or an index outside an operator's output; `reason` says why
		UNRESOLVED
	};
	Kind kind;
	//! The operator the step (or trace) stopped at
	const LogicalOperator *op;
	idx_t index;
	//! PASS_THROUGH only
	const LogicalOperator *next = nullptr;
	//! SCAN_COLUMN only
	column_t column_id = 0;
	//! UNRESOLVED only
	string reason;
};

static ReferenceStep Unresolved(const LogicalOperator &op, idx_t index, string reason) {
	ReferenceStep step {ReferenceStep::Kind::UNRESOLVED, &op, index};
	step.reason = std::move(reason);
	return step;
}

static ReferenceStep PassThrough(const LogicalOperator &op, idx_t index, idx_t child_index) {
	ReferenceStep step {ReferenceStep::Kind::PASS_THROUGH, &op, child_index};
	step.next = op.children[0].get();
	return step;
}

//! One step down from output column `index` of `op`. Throws "IN list rewritten into a join" for a MARK join over
//! constants that is not exactly an IN list (see ValidateInListJoin)
static ReferenceStep StepReference(const LogicalOperator &op, idx_t index) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		auto &column_ids = get.GetColumnIds();
		idx_t position = index;
		if (!get.projection_ids.empty()) {
			if (index >= get.projection_ids.size()) {
				return Unresolved(op, index, "reference outside the scan's output");
			}
			position = get.projection_ids[index];
		}
		if (position >= column_ids.size()) {
			return Unresolved(op, index, "reference outside the scan's output");
		}
		auto &column_index = column_ids[position];
		if (column_index.HasChildren()) {
			return Unresolved(op, index, "reference to a nested field pushed into the scan");
		}
		ReferenceStep step {ReferenceStep::Kind::SCAN_COLUMN, &op, index};
		step.column_id = column_index.GetPrimaryIndex();
		return step;
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto &filter = op.Cast<LogicalFilter>();
		idx_t child_index = index;
		if (!filter.projection_map.empty()) {
			if (index >= filter.projection_map.size()) {
				return Unresolved(op, index, "reference outside the filter's output");
			}
			child_index = filter.projection_map[index];
		}
		return PassThrough(op, index, child_index);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		if (index >= op.expressions.size()) {
			return Unresolved(op, index, "reference outside the projection's output");
		}
		auto &expr = *op.expressions[index];
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
			return PassThrough(op, index, expr.Cast<BoundReferenceExpression>().index);
		}
		return ReferenceStep {ReferenceStep::Kind::PROJECTION_EXPRESSION, &op, index};
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		if (!IsInListJoinCandidate(op)) {
			break;
		}
		auto &join = ValidateInListJoin(op);
		auto left_count = MarkJoinLeftCount(join);
		if (index == left_count) {
			return ReferenceStep {ReferenceStep::Kind::MARK_COLUMN, &op, index};
		}
		if (index > left_count) {
			return Unresolved(op, index, IN_LIST_JOIN);
		}
		return PassThrough(op, index, join.left_projection_map.empty() ? index : join.left_projection_map[index]);
	}
	default:
		break;
	}
	return Unresolved(op, index, StringUtil::Format("operator %s", LogicalOperatorToString(op.type)));
}

//! StepReference repeated through every PASS_THROUGH: where output column `index` of `op` really comes from
static ReferenceStep TraceReference(const LogicalOperator &op, idx_t index) {
	auto step = StepReference(op, index);
	while (step.kind == ReferenceStep::Kind::PASS_THROUGH) {
		step = StepReference(*step.next, step.index);
	}
	return step;
}

//! The IN-list MARK join whose mark column output column `index` of `op` is, passed through filters and projections
//! as a plain reference; null if it is anything else
static optional_ptr<const LogicalComparisonJoin> FindMarkJoin(const LogicalOperator &op, idx_t index) {
	auto traced = TraceReference(op, index);
	if (traced.kind != ReferenceStep::Kind::MARK_COLUMN) {
		return nullptr;
	}
	return &traced.op->Cast<LogicalComparisonJoin>();
}

//! A LogicalFilter expression over `input` that is exactly an IN-list mark column (IN) or NOT of one (NOT IN), as
//! `<x> [NOT] IN (<constants>)`; "" for any other expression. The mark is NULL when x is NULL (no NULL constants,
//! which are rejected), as is ClickhouseExpression::InList
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
	return ClickhouseExpression::InList(left, values, negated);
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
	auto step = StepReference(op, index);
	if (step.kind == ReferenceStep::Kind::PROJECTION_EXPRESSION ||
	    (step.kind == ReferenceStep::Kind::PASS_THROUGH && op.type == LogicalOperatorType::LOGICAL_PROJECTION)) {
		// a projection's output is translated (and parenthesized) as an expression, plain reference or not
		auto &child = *op.children[0];
		return "(" +
		       ClickhouseExpression::Translate(*op.expressions[index],
		                                       [&](idx_t i) { return ResolveOutput(child, i); }) +
		       ")";
	}
	switch (step.kind) {
	case ReferenceStep::Kind::PASS_THROUGH:
		return ResolveOutput(*step.next, step.index);
	case ReferenceStep::Kind::SCAN_COLUMN:
		return ColumnSql(ScanBindData(op.Cast<LogicalGet>()), step.column_id);
	case ReferenceStep::Kind::MARK_COLUMN:
		// the mark column anywhere but as a whole filter expression (or under a NOT), e.g. x IN (...) OR y
		throw NotImplementedException(IN_LIST_JOIN);
	default:
		throw NotImplementedException(step.reason);
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
		if (!bind_data.order_by_clause.empty() || !bind_data.limit_clause.empty()) {
			// never expected (a LIMIT or ORDER BY cannot be written in UPDATE/DELETE), but a scan limited in
			// ClickHouse would not match the rows the predicate alone selects
			throw NotImplementedException("scan with a pushed-down ORDER BY or LIMIT");
		}
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
		ThrowUnsupportedShape(statement, table, error.RawMessage());
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
	result.settings = ClickhouseConnection::ExtensionQuerySettings();
	if (target.predicate.empty()) {
		auto &engine = target.table.GetEngine();
		if (!TruncateRemovesRows(engine)) {
			throw NotImplementedException(
			    "DELETE without WHERE (or TRUNCATE) on ClickHouse table %s would run TRUNCATE TABLE, which does not "
			    "remove the rows of a table with engine %s; run the statement you need with clickhouse_execute() instead",
			    DisplayName(op.table), engine.empty() ? string("(unknown)") : engine);
		}
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
	auto traced = TraceReference(op, index);
	if (traced.kind != ReferenceStep::Kind::SCAN_COLUMN || IsVirtualColumn(traced.column_id)) {
		return optional_idx();
	}
	return traced.column_id;
}

//! `sql`, a TIMESTAMP WITH TIME ZONE, floored to `precision` digits of a second like an INSERT does
//! (ClickhouseUtils::ScaleTicks). ClickHouse's CAST to a coarser DateTime64 truncates toward zero instead, which
//! differs before 1970 (1969-12-31 23:59:59.9995 would become 1970-01-01 00:00:00.000); so would its
//! toStartOfMillisecond()
static string FloorTimestamp(const string &sql, idx_t precision) {
	auto micros = "toUnixTimestamp64Micro(CAST(" + sql + " AS " +
	              ClickhouseDdlTypes::ToClickhouse(LogicalType::TIMESTAMP_TZ, true) + "))";
	auto micros_per_tick = "1" + string(6 - precision, '0');
	return "fromUnixTimestamp64Micro(" + micros + " - positiveModulo(" + micros + ", " + micros_per_tick + "), 'UTC')";
}

//! Whether ClickHouse converts a value for `node`, or a type nested in it, on the server: the types it stores as
//! something other than the text DuckDB reads them as (IPv4/6, (U)Int256, Decimal256, FixedString, the geo types,
//! JSON, Variant, Dynamic, ...), whose CAST parses that text, and BFloat16, which DuckDB reads as FLOAT
static bool ConvertedOnServer(const ClickhouseTypeNode &node) {
	auto &base = ClickhouseTypeWrappers::Of(node).base;
	if (base.name == "String") {
		return false;
	}
	if (base.name == "BFloat16" || ClickhouseTypes::ToDuckDB(base).type.id() == LogicalTypeId::VARCHAR) {
		return true;
	}
	for (auto &child : base.children) {
		if (ConvertedOnServer(child)) {
			return true;
		}
	}
	return false;
}

//! Whether ClickHouse's parse of text into `base` (a type ConvertedOnServer) either gives back the value DuckDB
//! reads or fails: IPv4, IPv6 and FixedString. The others may wrap around, truncate or reinterpret the text
static bool ParsedFromTextExactly(const ClickhouseTypeNode &base) {
	return base.name == "IPv4" || base.name == "IPv6" || base.name == "FixedString";
}

//! The constant a SET value is, itself or as a projection's output below the LogicalUpdate (where DuckDB computes
//! the SET values); null for anything else
static optional_ptr<const BoundConstantExpression> ResolveConstant(const Expression &expr,
                                                                   const LogicalOperator &child) {
	auto resolved = &expr;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto traced = TraceReference(child, expr.Cast<BoundReferenceExpression>().index);
		if (traced.kind != ReferenceStep::Kind::PROJECTION_EXPRESSION) {
			return nullptr;
		}
		resolved = traced.op->expressions[traced.index].get();
	}
	if (resolved->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return nullptr;
	}
	return &resolved->Cast<BoundConstantExpression>();
}

//! Whether a column of this ClickHouse type can store NULL: Nullable (under LowCardinality too), Variant and Dynamic.
//! Not a plain scalar, nor Array, Tuple, Map or JSON, whose CAST of NULL fails
static bool CanHoldNull(const ClickhouseTypeNode &node) {
	auto &name = ClickhouseTypeWrappers::Of(node).base.name;
	return ClickhouseTypes::IsNullable(node) || name == "Variant" || name == "Dynamic";
}

//! The element types of an Array, Tuple or Map; empty for any other type
static const vector<ClickhouseTypeNode> &ElementTypes(const ClickhouseTypeNode &base) {
	static const vector<ClickhouseTypeNode> NONE;
	return base.name == "Array" || base.name == "Tuple" || base.name == "Map" ? base.children : NONE;
}

//! Whether a value of ClickHouse type `source` may hold a NULL element, at any level of Array, Tuple and Map, where
//! `target` cannot hold one. A null `source` (a computed value) may hold NULL anywhere, and so may a source whose
//! shape differs from the target's
static bool MayGetNullElement(const ClickhouseTypeNode &target, optional_ptr<const ClickhouseTypeNode> source) {
	auto &target_elements = ElementTypes(ClickhouseTypeWrappers::Of(target).base);
	optional_ptr<const vector<ClickhouseTypeNode>> source_elements;
	if (source) {
		auto &source_base = ClickhouseTypeWrappers::Of(*source).base;
		if (source_base.name == ClickhouseTypeWrappers::Of(target).base.name &&
		    ElementTypes(source_base).size() == target_elements.size()) {
			source_elements = &ElementTypes(source_base);
		}
	}
	for (idx_t i = 0; i < target_elements.size(); i++) {
		auto &element = target_elements[i];
		optional_ptr<const ClickhouseTypeNode> source_element;
		if (source_elements) {
			source_element = &(*source_elements)[i];
		}
		if (!CanHoldNull(element) && (!source_element || CanHoldNull(*source_element))) {
			return true;
		}
		if (MayGetNullElement(element, source_element)) {
			return true;
		}
	}
	return false;
}

//! Whether the constant `value` holds a NULL element, at any level of LIST, STRUCT and MAP, where ClickHouse type
//! `target` cannot hold one
static bool HoldsNullElement(const Value &value, const ClickhouseTypeNode &target) {
	if (value.IsNull()) {
		return false;
	}
	auto &target_elements = ElementTypes(ClickhouseTypeWrappers::Of(target).base);
	vector<std::pair<const Value *, const ClickhouseTypeNode *>> elements;
	switch (value.type().id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		if (target_elements.size() == 1) {
			auto &children = value.type().id() == LogicalTypeId::LIST ? ListValue::GetChildren(value)
			                                                          : ArrayValue::GetChildren(value);
			for (auto &child : children) {
				elements.emplace_back(&child, &target_elements[0]);
			}
		}
		break;
	case LogicalTypeId::STRUCT: {
		auto &children = StructValue::GetChildren(value);
		if (children.size() == target_elements.size()) {
			for (idx_t i = 0; i < children.size(); i++) {
				elements.emplace_back(&children[i], &target_elements[i]);
			}
		}
		break;
	}
	case LogicalTypeId::MAP:
		if (target_elements.size() == 2) {
			for (auto &entry : MapValue::GetChildren(value)) {
				auto &key_value = StructValue::GetChildren(entry);
				for (idx_t i = 0; i < key_value.size() && i < 2; i++) {
					elements.emplace_back(&key_value[i], &target_elements[i]);
				}
			}
		}
		break;
	default:
		break;
	}
	for (auto &element : elements) {
		if ((element.first->IsNull() && !CanHoldNull(*element.second)) ||
		    HoldsNullElement(*element.first, *element.second)) {
			return true;
		}
	}
	return false;
}

//! Whether a DateTime or DateTime64(p < 6) is nested in `node` (an Array, Tuple or Map element)
static bool HoldsCoarseDateTime(const ClickhouseTypeNode &node) {
	for (auto &child : node.children) {
		auto precision = ClickhouseTypes::DateTimePrecision(child);
		if ((precision.IsValid() && precision.GetIndex() < 6) || HoldsCoarseDateTime(child)) {
			return true;
		}
	}
	return false;
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
		ThrowUnsupportedShape("UPDATE", op.table, "SET list does not match its columns");
	}
	for (idx_t i = 0; i < op.columns.size(); i++) {
		if (op.columns[i].index >= columns.size()) {
			ThrowUnsupportedShape("UPDATE", op.table, "SET of an unknown column");
		}
		if (op.expressions[i]->GetExpressionClass() == ExpressionClass::BOUND_DEFAULT) {
			// ClickHouse has no DEFAULT in ALTER TABLE … UPDATE; checked before the matches_nothing and
			// unbound_parameters early returns, so such a statement is refused even when no row matches
			ThrowUnsupportedShape("UPDATE", op.table, "SET " + columns[op.columns[i].index].name + " = DEFAULT");
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
	vector<string> checked_values;
	vector<string> null_counts;
	try {
		for (idx_t i = 0; i < op.columns.size(); i++) {
			auto column_id = op.columns[i].index;
			auto &column = columns[column_id];
			auto &expr = *op.expressions[i];
			// whether the value must be checked for NULLs, which the column cannot hold: the mutation would fail on
			// the first one and stay stuck. A column that cannot hold NULL cannot give one either
			bool check_nulls = !CanHoldNull(column.type_node);
			optional_idx source;
			if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
				source = UnchangedScanColumn(child, expr.Cast<BoundReferenceExpression>().index);
				if (source.IsValid() && source.GetIndex() == column_id) {
					// col = col: DuckDB adds every column like this to some UPDATEs (e.g. of an array column, which
					// it turns into DELETE + INSERT); sending them would fail on key columns
					continue;
				}
				if (source.IsValid() && !CanHoldNull(columns[source.GetIndex()].type_node)) {
					check_nulls = false;
				}
			}
			auto constant = ResolveConstant(expr, child);
			// a NULL element also fails the mutation, and isNull() of an Array, Tuple or Map is never true: decided
			// here, from the constant or the ClickHouse type of the source column
			if (constant) {
				if (check_nulls && constant->value.IsNull()) {
					throw ConstraintException("NOT NULL constraint failed: %s.%s", target.table.name, column.name);
				}
				if (HoldsNullElement(constant->value, column.type_node)) {
					throw ConstraintException("NOT NULL constraint failed: %s.%s (an element of the new value is NULL, "
					                          "which ClickHouse type %s cannot store)",
					                          target.table.name, column.name, column.clickhouse_type);
				}
				check_nulls = false;
			} else if (source.IsValid()) {
				auto &source_column = columns[source.GetIndex()];
				if (MayGetNullElement(column.type_node, &source_column.type_node)) {
					throw NotImplementedException("SET of column \"%s\" (%s) to column \"%s\" (%s), whose elements can "
					                              "be NULL where the column's cannot",
					                              column.name, column.clickhouse_type, source_column.name,
					                              source_column.clickhouse_type);
				}
			} else if (MayGetNullElement(column.type_node, nullptr)) {
				throw NotImplementedException("SET of column \"%s\" (%s) to a computed value, whose elements could be "
				                              "NULL where the column's cannot",
				                              column.name, column.clickhouse_type);
			}
			if (ConvertedOnServer(column.type_node)) {
				auto &base = ClickhouseTypeWrappers::Of(column.type_node).base;
				if (constant && constant->value.IsNull()) {
					// exact whatever the type (a column that cannot hold NULL was refused above)
					assignments.push_back(ClickhouseUtils::QuoteIdentifier(column.name) + " = CAST(NULL AS " +
					                      column.clickhouse_type + ")");
					continue;
				}
				if (!ParsedFromTextExactly(base)) {
					// e.g. text out of an Int256's range wraps around, a Decimal256 truncates excess digits, a typed
					// JSON path truncates a number, a Variant keeps a JSON string's quotes, a BFloat16 is rounded
					throw NotImplementedException("SET of column \"%s\" (%s): ClickHouse converts the new value on the "
					                              "server, not always exactly",
					                              column.name, column.clickhouse_type);
				}
				// the mutation would parse the text: into NULL for a Nullable column, or failing, which leaves it
				// stuck. A constant is converted strictly, like an INSERT does, and checked first (check_sql)
				if (!constant) {
					throw NotImplementedException("SET of column \"%s\" (%s) to a value that is not a constant: "
					                              "ClickHouse parses it from text",
					                              column.name, column.clickhouse_type);
				}
				auto literal =
				    ClickhouseExpression::Translate(expr, [&](idx_t index) { return ResolveOutput(child, index); });
				auto assigned =
				    "CAST(" + ClickhouseWriter::ParseText(base, literal) + " AS " + column.clickhouse_type + ")";
				checked_values.push_back(assigned);
				assignments.push_back(ClickhouseUtils::QuoteIdentifier(column.name) + " = " + assigned);
				continue;
			}
			auto value =
			    ClickhouseExpression::Translate(expr, [&](idx_t index) { return ResolveOutput(child, index); });
			// DuckDB's binder casts every SET value to the column's DuckDB type, and that cast is part of `expr`
			// (translated, and rounded where DuckDB rounds, above). The CAST to the ClickHouse type below then
			// converts the column's DuckDB type into its ClickHouse type: exact for the types left here, except for
			// DateTime and DateTime64(p < 6), floored first like an INSERT. Should the value's type ever differ from
			// the column's, that first cast is checked (and rounded) here
			if (expr.return_type != column.type) {
				value = ClickhouseExpression::Cast(value, expr.return_type, column.type,
				                                   ClickhouseDdlTypes::ToClickhouse(column.type, true));
			}
			auto precision = ClickhouseTypes::DateTimePrecision(column.type_node);
			if (precision.IsValid() && precision.GetIndex() < 6) {
				value = FloorTimestamp(value, precision.GetIndex());
			} else if (HoldsCoarseDateTime(column.type_node)) {
				throw NotImplementedException(
				    "SET of column \"%s\" (%s), whose DateTime elements ClickHouse's CAST would truncate toward zero "
				    "where an INSERT floors them",
				    column.name, column.clickhouse_type);
			}
			if (check_nulls) {
				null_counts.push_back("countIf(isNull(" + value + "))");
				result.null_check_columns.push_back(column.name);
			}
			assignments.push_back(ClickhouseUtils::QuoteIdentifier(column.name) + " = CAST(" + value + " AS " +
			                      column.clickhouse_type + ")");
		}
	} catch (NotImplementedException &ex) {
		ErrorData error(ex);
		ThrowUnsupportedShape("UPDATE", op.table, error.RawMessage());
	}
	auto where = target.predicate.empty() ? string("1") : target.predicate;
	result.count_sql = "SELECT count() FROM " + target.qualified_name + " WHERE " + where;
	if (assignments.empty()) {
		// nothing changes: count only
		return result;
	}
	result.sql = result.description + " " + StringUtil::Join(assignments, ", ") + " WHERE " + where;
	if (!checked_values.empty()) {
		// ignore() still evaluates its arguments, and returns a type the client can read (Int256 and Point are not)
		result.check_sql = "SELECT ignore(" + StringUtil::Join(checked_values, ", ") + ")";
	}
	if (!null_counts.empty()) {
		result.null_check_sql =
		    "SELECT " + StringUtil::Join(null_counts, ", ") + " FROM " + target.qualified_name + " WHERE " + where;
		result.table_name = target.table.name;
	}
	result.settings = ClickhouseConnection::ExtensionQuerySettings();
	result.settings.push_back(MutationsSyncSetting(context));
	return result;
}

std::pair<string, string> ClickhouseDml::MutationsSyncSetting(ClientContext &context) {
	Value mutations_sync;
	string sync = "2";
	if (context.TryGetCurrentSetting("ch_mutations_sync", mutations_sync) && !mutations_sync.IsNull()) {
		sync = mutations_sync.ToString();
	}
	return {"mutations_sync", sync};
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
		if (!statement.check_sql.empty()) {
			// the same conversions as the statement's: one that fails throws ClickHouse's error here
			connection->Query(statement.check_sql, ClickhouseConnection::ExtensionQuerySettings());
		}
		if (!statement.null_check_sql.empty()) {
			auto settings = ClickhouseConnection::ExtensionQuerySettings();
			settings.emplace_back("apply_deleted_mask", "0");
			for (auto &block : connection->Query(statement.null_check_sql, settings)) {
				if (block.GetRowCount() == 0) {
					continue;
				}
				if (block.GetColumnCount() != statement.null_check_columns.size()) {
					throw InvalidInputException("%s: unexpected result for the NULL check (%s)", statement.description,
					                            statement.null_check_sql);
				}
				for (idx_t i = 0; i < statement.null_check_columns.size(); i++) {
					auto nulls = block[i]->As<clickhouse::ColumnUInt64>();
					if (!nulls) {
						throw InvalidInputException("%s: unexpected result for the NULL check (%s)",
						                            statement.description, statement.null_check_sql);
					}
					if (nulls->At(0) > 0) {
						throw ConstraintException(
						    "NOT NULL constraint failed: %s.%s (the new value is NULL in %d row(s) the WHERE clause "
						    "matches, counting deleted rows not purged yet: ALTER TABLE … APPLY DELETED MASK purges "
						    "those); nothing was changed",
						    statement.table_name, statement.null_check_columns[i], nulls->At(0));
					}
				}
			}
		}
		// the count is an ordinary query, which an ATTACH's settings= would reach without ExtensionQuerySettings
		for (auto &block : connection->Query(statement.count_sql, ClickhouseConnection::ExtensionQuerySettings())) {
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
