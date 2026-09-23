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
#include "duckdb/planner/filter/optional_filter.hpp"

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
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return value.ToString();
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

//! Whether expr is `CAST(<column reference> AS VARCHAR)`
static bool IsVarcharCastOfColumnReference(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_CAST) {
		return false;
	}
	auto &cast_expr = expr.Cast<BoundCastExpression>();
	return cast_expr.return_type.id() == LogicalTypeId::VARCHAR &&
	      cast_expr.child->GetExpressionClass() == ExpressionClass::BOUND_REF;
}

string ClickhouseFilterPushdown::TransformFilter(const string &column, const TableFilter &filter, bool optional) {
	switch (filter.filter_type) {
	case TableFilterType::IS_NULL:
		return column + " IS NULL";
	case TableFilterType::IS_NOT_NULL:
		return column + " IS NOT NULL";
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = filter.Cast<ConstantFilter>();
		return column + " " + TransformComparison(constant_filter.comparison_type) + " " +
		       TransformConstant(constant_filter.constant);
	}
	case TableFilterType::IN_FILTER: {
		auto &in_filter = filter.Cast<InFilter>();
		vector<string> values;
		for (auto &value : in_filter.values) {
			values.push_back(TransformConstant(value));
		}
		return column + " IN (" + StringUtil::Join(values, ", ") + ")";
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction = filter.Cast<ConjunctionAndFilter>();
		vector<string> parts;
		for (auto &child : conjunction.child_filters) {
			auto part = TransformFilter(column, *child, optional);
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
			auto part = TransformFilter(column, *child, optional);
			if (part.empty()) {
				// one branch does not restrict rows, so neither does the OR
				return string();
			}
			parts.push_back(part);
		}
		return "(" + StringUtil::Join(parts, " OR ") + ")";
	}
	case TableFilterType::OPTIONAL_FILTER: {
		auto &optional_filter = filter.Cast<OptionalFilter>();
		if (!optional_filter.child_filter) {
			return string();
		}
		try {
			return TransformFilter(column, *optional_filter.child_filter, true);
		} catch (NotImplementedException &) {
			return string();
		}
	}
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
				auto op = expr.GetExpressionType() == ExpressionType::COMPARE_EQUAL ? " = " : " != ";
				return column + op + ClickhouseUtils::QuoteLiteral(StringValue::Get(constant.value));
			}
		}
		if (optional) {
			return string();
		}
		throw NotImplementedException("ClickHouse filter pushdown: unsupported expression filter %s",
		                              expr.ToString());
	}
	default:
		if (optional) {
			return string();
		}
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
