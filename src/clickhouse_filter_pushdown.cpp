#include "clickhouse_filter_pushdown.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/scalar/tablefilter_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter_set.hpp"

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

static bool IsOrderedComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return true;
	default:
		return false;
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
	case LogicalTypeId::UUID:
		return "toUUID(" + ClickhouseUtils::QuoteLiteral(value.ToString()) + ")";
	case LogicalTypeId::DATE: {
		auto date = DateValue::Get(value);
		if (!date.IsFinite()) {
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
		if (!timestamp.IsFinite()) {
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
		if (!date.IsFinite()) {
			return FoldOutOfBoundsComparison(column, comparison_type, date == date_t::ninfinity());
		}
		if (!DateInPushdownRange(date)) {
			return FoldOutOfBoundsComparison(column, comparison_type, date < Date::FromDate(1900, 1, 1));
		}
		return string();
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		auto timestamp = TimestampTZValue::Get(value);
		if (!timestamp.IsFinite()) {
			return FoldOutOfBoundsComparison(column, comparison_type, timestamp == timestamp_tz_t::ninfinity());
		}
		return string();
	}
	default:
		return string();
	}
}

//! Whether expr is `CAST(<column reference> AS VARCHAR)`
static bool IsVarcharCastOfColumnReference(const Expression &expr) {
	if (!BoundCastExpression::IsCast(expr)) {
		return false;
	}
	auto &cast_expr = expr.Cast<BoundFunctionExpression>();
	if (cast_expr.GetReturnType().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	auto child_class = BoundCastExpression::Child(cast_expr).GetExpressionClass();
	return child_class == ExpressionClass::BOUND_REF || child_class == ExpressionClass::BOUND_COLUMN_REF;
}

static bool IsColumnSubject(const Expression &expr) {
	return ExpressionFilter::IsSimpleFilterColumnRef(expr);
}

static bool IsRuntimeHintExpression(const Expression &expr) {
	return ExpressionFilter::ContainsInternalFunction(expr, TableFilterDynamicFun::Name) ||
	       ExpressionFilter::ContainsInternalFunction(expr, TableFilterBloomFilterFun::Name) ||
	       ExpressionFilter::ContainsInternalFunction(expr, TableFilterPrefixRangeFun::Name);
}

static string TransformExpressionFilter(const string &column, const Expression &expr);

string ClickhouseFilterPushdown::TransformFilter(const string &column, const TableFilter &filter) {
	return TransformFilter(column, filter, LogicalType::INVALID);
}

string ClickhouseFilterPushdown::TransformFilter(const string &column, const TableFilter &filter,
                                                 const LogicalType &column_type) {
	if (filter.filter_type == TableFilterType::EXPRESSION_FILTER) {
		if (ExpressionFilter::IsOptionalFilter(filter) || ExpressionFilter::IsRootOptionalFilter(filter)) {
			return string();
		}
		if (ExpressionFilter::GetRootOptionalDynamicFilterData(filter)) {
			return string();
		}
		return TransformExpressionFilter(column, *ExpressionFilter::GetExpressionFilter(filter, "clickhouse").expr);
	}
	switch (filter.filter_type) {
	case TableFilterType::LEGACY_OPTIONAL_FILTER:
		return string();
	case TableFilterType::LEGACY_DYNAMIC_FILTER:
	case TableFilterType::LEGACY_BLOOM_FILTER:
	case TableFilterType::LEGACY_PREFIX_RANGE_FILTER:
		return string();
	default:
		break;
	}
	auto type = column_type.IsValid() ? column_type : LogicalType::VARCHAR;
	auto converted = ExpressionFilter::FromTableFilter(filter, type);
	return TransformExpressionFilter(column, *converted->expr);
}

static string TransformExpressionFilter(const string &column, const Expression &expr) {
	if (ExpressionFilter::IsOptionalExpression(expr) || ExpressionFilter::IsRootOptionalExpression(expr)) {
		return string();
	}
	if (IsRuntimeHintExpression(expr) && expr.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return string();
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		auto is_and = expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND;
		vector<string> parts;
		for (auto &child : conj.GetChildren()) {
			auto part = TransformExpressionFilter(column, *child);
			if (is_and) {
				if (!part.empty()) {
					parts.push_back(part);
				}
			} else if (part.empty()) {
				// one branch does not restrict rows, so neither does the OR
				return string();
			} else {
				parts.push_back(part);
			}
		}
		if (parts.empty()) {
			return string();
		}
		return "(" + StringUtil::Join(parts, is_and ? " AND " : " OR ") + ")";
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &op_expr = expr.Cast<BoundOperatorExpression>();
		auto &children = op_expr.GetChildren();
		if (children.size() == 1 && IsColumnSubject(*children[0])) {
			if (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NULL) {
				return column + " IS NULL";
			}
			if (expr.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL) {
				return column + " IS NOT NULL";
			}
		}
		if (expr.GetExpressionType() == ExpressionType::COMPARE_IN ||
		    expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN) {
			vector<string> values;
			for (idx_t i = 1; i < children.size(); i++) {
				if (children[i]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
					throw NotImplementedException("ClickHouse filter pushdown: unsupported expression filter %s",
					                              expr.ToString());
				}
				auto &value = children[i]->Cast<BoundConstantExpression>().GetValue();
				try {
					values.push_back(ClickhouseFilterPushdown::TransformConstant(value));
				} catch (NotImplementedException &) {
					// TransformConstant refuses a value of a type it otherwise translates in exactly two cases:
					// a DATE outside the range toDate32 represents exactly, and an infinite DATE or
					// TIMESTAMP_TZ. Such a value can never equal any value the column could hold, so dropping
					// it from the list is exact.
					auto type_id = value.type().id();
					if (type_id != LogicalTypeId::DATE && type_id != LogicalTypeId::TIMESTAMP_TZ) {
						throw;
					}
				}
			}
			if (values.empty()) {
				return expr.GetExpressionType() == ExpressionType::COMPARE_IN ? "1 = 0" : "1 = 1";
			}
			auto op = expr.GetExpressionType() == ExpressionType::COMPARE_IN ? " IN " : " NOT IN ";
			return column + op + "(" + StringUtil::Join(values, ", ") + ")";
		}
	}
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto &comparison = expr.Cast<BoundFunctionExpression>();
		auto comparison_type = expr.GetExpressionType();
		auto constant_ptr = ExpressionFilter::TryGetColumnConstantComparison(comparison, comparison_type);
		if (constant_ptr) {
			auto &constant = constant_ptr->GetValue();
			auto folded = TryFoldUnrepresentableComparison(column, comparison_type, constant);
			if (!folded.empty()) {
				return folded;
			}
			if (constant.type().id() == LogicalTypeId::UUID && IsOrderedComparison(comparison_type)) {
				// ClickHouse orders UUIDs by their second 64-bit half first; its canonical lowercase text orders
				// exactly like DuckDB's UUID
				return "toString(" + column + ") " + TransformComparison(comparison_type) + " " +
				       ClickhouseUtils::QuoteLiteral(constant.ToString());
			}
			return column + " " + TransformComparison(comparison_type) + " " +
			       ClickhouseFilterPushdown::TransformConstant(constant);
		}
		if (comparison_type == ExpressionType::COMPARE_EQUAL || comparison_type == ExpressionType::COMPARE_NOTEQUAL) {
			auto &left = BoundComparisonExpression::Left(comparison);
			auto &right = BoundComparisonExpression::Right(comparison);
			const Expression *other = nullptr;
			if (IsVarcharCastOfColumnReference(left)) {
				other = &right;
			} else if (IsVarcharCastOfColumnReference(right)) {
				other = &left;
			}
			if (other && other->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				auto &constant = other->Cast<BoundConstantExpression>();
				if (!constant.GetValue().IsNull() && constant.GetValue().type().id() == LogicalTypeId::VARCHAR) {
					auto op = comparison_type == ExpressionType::COMPARE_EQUAL ? " = " : " != ";
					return column + op + ClickhouseUtils::QuoteLiteral(StringValue::Get(constant.GetValue()));
				}
			}
		}
	}
	throw NotImplementedException("ClickHouse filter pushdown: unsupported expression filter %s", expr.ToString());
}

string ClickhouseFilterPushdown::TransformFilters(const vector<column_t> &column_ids,
                                                  optional_ptr<TableFilterSet> filters,
                                                  const vector<ClickhouseColumnInfo> &columns) {
	if (!filters || !filters->HasFilters()) {
		return string();
	}
	vector<string> conditions;
	for (auto &entry : *filters) {
		auto column_id = column_ids[entry.GetIndex()];
		if (IsVirtualColumn(column_id)) {
			throw InternalException("ClickHouse filter pushdown: unexpected filter on a virtual column");
		}
		auto column = ClickhouseUtils::QuoteIdentifier(columns[column_id].name);
		auto condition = TransformFilter(column, entry.Filter(), columns[column_id].type);
		if (!condition.empty()) {
			conditions.push_back(condition);
		}
	}
	return StringUtil::Join(conditions, " AND ");
}

} // namespace duckdb
