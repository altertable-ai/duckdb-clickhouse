#include "clickhouse_filter_pushdown.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"

namespace duckdb {

static string TransformComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "!=";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		throw NotImplementedException("ClickHouse filter pushdown: unsupported comparison %s",
		                              EnumUtil::ToString(type));
	}
}

//! toDate32(...) clamps rather than errors on an out-of-range argument (e.g. toDate32('1850-01-01') silently
//! becomes 1900-01-01): pushing a comparison against a date outside this range would then match/exclude rows
//! ClickHouse itself would not. [1900-01-01, 2299-12-31] is ClickHouse's own Date32 range -- the widest either
//! Date or Date32 (whichever the filtered column actually is) can ever store, so an in-range check here is safe
//! regardless of which of the two the column turns out to be (Date's own range, 1970-01-01..2149-06-06, is a
//! subset of it).
static bool DateInPushdownRange(date_t date) {
	static const date_t MIN_PUSHDOWN_DATE = Date::FromDate(1900, 1, 1);
	static const date_t MAX_PUSHDOWN_DATE = Date::FromDate(2299, 12, 31);
	return date >= MIN_PUSHDOWN_DATE && date <= MAX_PUSHDOWN_DATE;
}

string ClickhouseFilterPushdown::TransformConstant(const Value &value) {
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return BooleanValue::Get(value) ? "true" : "false";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return value.ToString();
	case LogicalTypeId::HUGEINT:
		// beyond +-2^63, a bare integer literal parses as Float64 in ClickHouse (losing precision) instead of
		// comparing exactly against an Int128 column
		return "toInt128(" + ClickhouseUtils::QuoteLiteral(value.ToString()) + ")";
	case LogicalTypeId::UHUGEINT:
		return "toUInt128(" + ClickhouseUtils::QuoteLiteral(value.ToString()) + ")";
	case LogicalTypeId::DECIMAL:
		// a plain 12.5 literal would be a Float64 in ClickHouse, which does not compare with Decimal columns
		return StringUtil::Format("toDecimal128(%s, %d)", ClickhouseUtils::QuoteLiteral(value.ToString()),
		                          static_cast<int32_t>(DecimalType::GetScale(value.type())));
	case LogicalTypeId::VARCHAR:
		return ClickhouseUtils::QuoteLiteral(StringValue::Get(value));
	case LogicalTypeId::ENUM:
		return ClickhouseUtils::QuoteLiteral(value.ToString());
	case LogicalTypeId::DATE: {
		auto date = DateValue::Get(value);
		if (!Date::IsFinite(date)) {
			throw NotImplementedException("ClickHouse filter pushdown: infinite dates are not supported");
		}
		if (!DateInPushdownRange(date)) {
			// CONSTANT_COMPARISON folds this to an exact predicate before ever calling TransformConstant (see
			// TryFoldUnrepresentableComparison); reachable here only from IN_FILTER, which drops values that
			// throw NotImplementedException instead of mistranslating them
			throw NotImplementedException("ClickHouse filter pushdown: date %s is outside the range toDate32 "
			                              "can represent exactly",
			                              value.ToString());
		}
		return "toDate32(" + ClickhouseUtils::QuoteLiteral(Date::ToString(date)) + ")";
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		auto timestamp = TimestampTZValue::Get(value);
		if (!Timestamp::IsFinite(timestamp)) {
			throw NotImplementedException("ClickHouse filter pushdown: infinite timestamps are not supported");
		}
		return StringUtil::Format("fromUnixTimestamp64Micro(%d, 'UTC')", timestamp.value);
	}
	default:
		throw NotImplementedException("ClickHouse filter pushdown: unsupported constant type %s",
		                              value.type().ToString());
	}
}

//! `column OP value` where value is provably outside every value column could ever hold (+-infinity, or --
//! for Date/Date32 -- outside ClickHouse's representable range): either the comparison can never be true for any
//! row (folds to a predicate that is false whether or not column itself is NULL, matching DuckDB's own
//! NULL-excluding WHERE semantics either way) or it is true for every non-NULL row (folds to `column IS NOT
//! NULL`).
static string FoldOutOfBoundsComparison(const string &column, ExpressionType comparison_type,
                                        bool value_is_below_range) {
	bool always_true_when_not_null;
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		always_true_when_not_null = false;
		break;
	case ExpressionType::COMPARE_NOTEQUAL:
		always_true_when_not_null = true;
		break;
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		always_true_when_not_null = !value_is_below_range;
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		always_true_when_not_null = value_is_below_range;
		break;
	default:
		throw NotImplementedException("ClickHouse filter pushdown: unsupported comparison %s",
		                              EnumUtil::ToString(comparison_type));
	}
	return always_true_when_not_null ? column + " IS NOT NULL" : "1 = 0";
}

//! Folds a comparison against a Date/Date32 or TIMESTAMPTZ constant ClickHouse cannot represent exactly (an
//! out-of-range Date, or +-infinity) to an equivalent predicate, instead of pushing a mistranslated constant
//! (Date) or failing the whole query (both currently throw in TransformConstant). Returns "" when value is
//! representable and the normal `column OP TransformConstant(value)` path applies.
static string TryFoldUnrepresentableComparison(const string &column, ExpressionType comparison_type,
                                               const Value &value) {
	switch (value.type().id()) {
	case LogicalTypeId::DATE: {
		auto date = DateValue::Get(value);
		if (!Date::IsFinite(date)) {
			return FoldOutOfBoundsComparison(column, comparison_type, date == date_t::ninfinity());
		}
		if (!DateInPushdownRange(date)) {
			return FoldOutOfBoundsComparison(column, comparison_type, date < Date::FromDate(1900, 1, 1));
		}
		return string();
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		auto timestamp = TimestampTZValue::Get(value);
		if (!Timestamp::IsFinite(timestamp)) {
			return FoldOutOfBoundsComparison(column, comparison_type, timestamp == timestamp_t::ninfinity());
		}
		return string();
	}
	default:
		return string();
	}
}

//! Whether expr is `CAST(<column reference> AS VARCHAR)`
static bool IsVarcharCastOfColumnReference(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return false;
	}
	auto &cast_expr = expr.Cast<BoundCastExpression>();
	return cast_expr.return_type.id() == LogicalTypeId::VARCHAR &&
	      cast_expr.child->GetExpressionClass() == ExpressionClass::BOUND_REF;
}

string ClickhouseFilterPushdown::TransformFilter(const string &column, const TableFilter &filter) {
	switch (filter.filter_type) {
	case TableFilterType::IS_NULL:
		return column + " IS NULL";
	case TableFilterType::IS_NOT_NULL:
		return column + " IS NOT NULL";
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = filter.Cast<ConstantFilter>();
		auto folded = TryFoldUnrepresentableComparison(column, constant_filter.comparison_type,
		                                                constant_filter.constant);
		if (!folded.empty()) {
			return folded;
		}
		return column + " " + TransformComparison(constant_filter.comparison_type) + " " +
		       TransformConstant(constant_filter.constant);
	}
	case TableFilterType::IN_FILTER: {
		auto &in_filter = filter.Cast<InFilter>();
		vector<string> values;
		for (auto &value : in_filter.values) {
			try {
				values.push_back(TransformConstant(value));
			} catch (NotImplementedException &) {
				// TransformConstant refuses a value of a type it otherwise translates in exactly two cases:
				// a DATE outside the range toDate32 represents exactly, and an infinite DATE or
				// TIMESTAMP_TZ. Such a value can never equal any value the column could hold, so dropping
				// it from the list is exact. A refusal for any other type means the type is not translatable
				// at all, which would make dropping it silently wrong -- rethrow and let the whole filter
				// stay with DuckDB.
				auto type_id = value.type().id();
				if (type_id != LogicalTypeId::DATE && type_id != LogicalTypeId::TIMESTAMP_TZ) {
					throw;
				}
			}
		}
		if (values.empty()) {
			return "1 = 0";
		}
		return column + " IN (" + StringUtil::Join(values, ", ") + ")";
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction = filter.Cast<ConjunctionAndFilter>();
		vector<string> parts;
		for (auto &child : conjunction.child_filters) {
			auto part = TransformFilter(column, *child);
			if (!part.empty()) {
				parts.push_back(part);
			}
		}
		if (parts.empty()) {
			return string();
		}
		return "(" + StringUtil::Join(parts, " AND ") + ")";
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction = filter.Cast<ConjunctionOrFilter>();
		vector<string> parts;
		for (auto &child : conjunction.child_filters) {
			auto part = TransformFilter(column, *child);
			if (part.empty()) {
				// one branch does not restrict rows, so neither does the OR
				return string();
			}
			parts.push_back(part);
		}
		return "(" + StringUtil::Join(parts, " OR ") + ")";
	}
	case TableFilterType::OPTIONAL_FILTER:
		// "executing filter is not required for query correctness" (table_filter.hpp) -- DuckDB keeps enforcing
		// the real predicate itself wherever it needs to (above a join, or above the LIMIT/ORDER BY this filter
		// was pushed alongside), so it must never become a ClickHouse predicate: doing so could exclude rows
		// before that real operator ever sees them (a join whose build side resolves to one value, feeding a
		// LIMIT, is the shape that reproduces it). Never translate the wrapped filter, regardless of whether
		// it is itself translatable.
		return string();
	case TableFilterType::DYNAMIC_FILTER:
	case TableFilterType::BLOOM_FILTER:
		// runtime hints from joins / top-n: the operators above the scan still enforce them
		return string();
	case TableFilterType::EXPRESSION_FILTER: {
		// the only two expression shapes ClickhousePushdownExpression (clickhouse_scanner.cpp) ever hands back:
		// a unary IS [NOT] NULL directly on a bound column reference, or `CAST(<enum column> AS VARCHAR) =/!=
		// <string constant>` (DuckDB's binder rewrite of `enum_col =/!= 'literal'`)
		auto &expression_filter = filter.Cast<ExpressionFilter>();
		auto &expr = *expression_filter.expr;
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
			auto &op_expr = expr.Cast<BoundOperatorExpression>();
			if (op_expr.children.size() == 1 && op_expr.children[0]->GetExpressionClass() == ExpressionClass::BOUND_REF) {
				if (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL) {
					return column + " IS NULL";
				}
				if (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
					return column + " IS NOT NULL";
				}
			}
		} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_COMPARISON &&
		          (expr.GetExpressionType() == ExpressionType::COMPARE_EQUAL ||
		           expr.GetExpressionType() == ExpressionType::COMPARE_NOTEQUAL)) {
			auto &comparison = expr.Cast<BoundComparisonExpression>();
			const Expression *other = nullptr;
			if (IsVarcharCastOfColumnReference(*comparison.left)) {
				other = comparison.right.get();
			} else if (IsVarcharCastOfColumnReference(*comparison.right)) {
				other = comparison.left.get();
			}
			if (other && other->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				auto &constant = other->Cast<BoundConstantExpression>();
				// ClickhousePushdownExpression already rejects a NULL or non-VARCHAR constant here (and
				// validates the label against the column's own enum members) -- checked again defensively
				// since this switch has no other way to know that gate ran
				if (!constant.value.IsNull() && constant.value.type().id() == LogicalTypeId::VARCHAR) {
					auto op = expr.GetExpressionType() == ExpressionType::COMPARE_EQUAL ? " = " : " != ";
					return column + op + ClickhouseUtils::QuoteLiteral(StringValue::Get(constant.value));
				}
			}
		}
		throw NotImplementedException("ClickHouse filter pushdown: unsupported expression filter %s",
		                              expr.ToString());
	}
	default:
		throw NotImplementedException("ClickHouse filter pushdown: unsupported filter type %s",
		                              EnumUtil::ToString(filter.filter_type));
	}
}

string ClickhouseFilterPushdown::TransformFilters(const vector<column_t> &column_ids,
                                                  optional_ptr<TableFilterSet> filters,
                                                  const vector<ClickhouseColumnInfo> &columns) {
	if (!filters || filters->filters.empty()) {
		return string();
	}
	vector<string> conditions;
	for (auto &entry : filters->filters) {
		auto column_id = column_ids[entry.first];
		if (IsVirtualColumn(column_id)) {
			throw InternalException("ClickHouse filter pushdown: unexpected filter on a virtual column");
		}
		auto column = ClickhouseUtils::QuoteIdentifier(columns[column_id].name);
		auto condition = TransformFilter(column, *entry.second);
		if (!condition.empty()) {
			conditions.push_back(condition);
		}
	}
	return StringUtil::Join(conditions, " AND ");
}

} // namespace duckdb
