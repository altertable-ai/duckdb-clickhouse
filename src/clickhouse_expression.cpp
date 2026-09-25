#include "clickhouse_expression.hpp"

#include "clickhouse_ddl_types.hpp"
#include "clickhouse_filter_pushdown.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/table_filter.hpp"

#include <cmath>

namespace duckdb {

[[noreturn]] static void ThrowUntranslatable(const Expression &expr) {
	throw NotImplementedException("untranslatable expression: %s", expr.ToString());
}

//! A FLOAT/DOUBLE value as a ClickHouse Float64 literal (nan and inf included)
static string FloatingPointLiteral(const Value &value) {
	auto number = value.GetValue<double>();
	if (std::isnan(number)) {
		return "nan";
	}
	if (std::isinf(number)) {
		return number > 0 ? "inf" : "-inf";
	}
	return value.ToString();
}

string ClickhouseExpression::Literal(const Value &value) {
	if (value.IsNull()) {
		return "NULL";
	}
	switch (value.type().id()) {
	case LogicalTypeId::FLOAT:
		// a bare 0.1 is a Float64 in ClickHouse, which does not equal the Float32 0.1 a REAL column holds
		return "toFloat32(" + FloatingPointLiteral(value) + ")";
	case LogicalTypeId::DOUBLE:
		return FloatingPointLiteral(value);
	case LogicalTypeId::UUID:
		return "toUUID(" + ClickhouseUtils::QuoteLiteral(value.ToString()) + ")";
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
		// naive timestamps are stored as UTC (see ClickhouseDdlTypes)
		return ClickhouseFilterPushdown::TransformConstant(
		    Value::TIMESTAMPTZ(timestamp_tz_t(value.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>())));
	default:
		return ClickhouseFilterPushdown::TransformConstant(value);
	}
}

//! TIMESTAMP WITH TIME ZONE and TIME WITH TIME ZONE: every cast to or from another type (DATE, TIMESTAMP, VARCHAR,
//! TIME, ...) goes through a time zone
static bool IsTimeZoneDependent(const LogicalType &type) {
	return type.id() == LogicalTypeId::TIMESTAMP_TZ || type.id() == LogicalTypeId::TIME_TZ;
}

static bool IsNullConstant(const Expression &expr) {
	return expr.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
	       expr.Cast<BoundConstantExpression>().value.IsNull();
}

static string ComparisonOperator(const Expression &expr) {
	switch (expr.GetExpressionType()) {
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
		ThrowUntranslatable(expr);
	}
}

//! A constant LIKE pattern: DuckDB has no default escape character, ClickHouse treats backslash as one
static string LikePattern(const Expression &pattern) {
	if (pattern.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		ThrowUntranslatable(pattern);
	}
	auto &value = pattern.Cast<BoundConstantExpression>().value;
	if (value.IsNull() || value.type().id() != LogicalTypeId::VARCHAR) {
		ThrowUntranslatable(pattern);
	}
	return ClickhouseUtils::QuoteLiteral(StringUtil::Replace(StringValue::Get(value), "\\", "\\\\"));
}

static string TranslateFunction(const BoundFunctionExpression &function, const std::function<string(idx_t)> &resolve) {
	auto &name = function.function.name;
	auto &children = function.children;
	auto arg = [&](idx_t i) {
		return ClickhouseExpression::Translate(*children[i], resolve);
	};
	auto is_string = [&](idx_t i) {
		return children[i]->return_type.id() == LogicalTypeId::VARCHAR;
	};
	if (children.size() == 2 && (name == "+" || name == "-" || name == "*" || name == "/" || name == "%")) {
		return "(" + arg(0) + " " + name + " " + arg(1) + ")";
	}
	if (children.size() == 1 && name == "-") {
		return "(-" + arg(0) + ")";
	}
	if (children.size() == 2 && name == "//") {
		return "intDiv(" + arg(0) + ", " + arg(1) + ")";
	}
	if (children.size() == 2 && (name == "~~" || name == "!~~" || name == "~~*" || name == "!~~*")) {
		auto negated = name[0] == '!';
		auto keyword = StringUtil::EndsWith(name, "*") ? "ILIKE" : "LIKE";
		return "(" + arg(0) + (negated ? " NOT " : " ") + keyword + " " + LikePattern(*children[1]) + ")";
	}
	if (children.size() == 2 && (name == "starts_with" || name == "prefix")) {
		return "startsWith(" + arg(0) + ", " + arg(1) + ")";
	}
	if (children.size() == 2 && (name == "ends_with" || name == "suffix")) {
		return "endsWith(" + arg(0) + ", " + arg(1) + ")";
	}
	if (children.size() == 2 && name == "contains" && is_string(0) && is_string(1)) {
		return "(position(" + arg(0) + ", " + arg(1) + ") > 0)";
	}
	if (children.size() == 1 && (name == "lower" || name == "lcase")) {
		return "lowerUTF8(" + arg(0) + ")";
	}
	if (children.size() == 1 && (name == "upper" || name == "ucase")) {
		return "upperUTF8(" + arg(0) + ")";
	}
	if (children.size() == 1 && (name == "length" || name == "len") && is_string(0)) {
		return "lengthUTF8(" + arg(0) + ")";
	}
	ThrowUntranslatable(function);
}

static string TranslateOperator(const BoundOperatorExpression &op, const std::function<string(idx_t)> &resolve) {
	auto arg = [&](idx_t i) {
		return ClickhouseExpression::Translate(*op.children[i], resolve);
	};
	switch (op.GetExpressionType()) {
	case ExpressionType::OPERATOR_NOT:
		if (op.children.size() != 1) {
			ThrowUntranslatable(op);
		}
		return "(NOT " + arg(0) + ")";
	case ExpressionType::OPERATOR_IS_NULL:
		if (op.children.size() != 1) {
			ThrowUntranslatable(op);
		}
		return "(" + arg(0) + " IS NULL)";
	case ExpressionType::OPERATOR_IS_NOT_NULL:
		if (op.children.size() != 1) {
			ThrowUntranslatable(op);
		}
		return "(" + arg(0) + " IS NOT NULL)";
	case ExpressionType::COMPARE_IN:
	case ExpressionType::COMPARE_NOT_IN: {
		if (op.children.size() < 2) {
			ThrowUntranslatable(op);
		}
		vector<string> values;
		for (idx_t i = 1; i < op.children.size(); i++) {
			auto &child = *op.children[i];
			// NULLs in the list: DuckDB yields NULL where ClickHouse (transform_null_in=0) yields 0/1, which flips
			// NOT IN; only constant, non-NULL lists translate exactly
			if (child.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
			    child.Cast<BoundConstantExpression>().value.IsNull()) {
				ThrowUntranslatable(op);
			}
			values.push_back(arg(i));
		}
		auto keyword = op.GetExpressionType() == ExpressionType::COMPARE_IN ? " IN (" : " NOT IN (";
		return "(" + arg(0) + keyword + StringUtil::Join(values, ", ") + "))";
	}
	case ExpressionType::OPERATOR_COALESCE: {
		vector<string> values;
		for (idx_t i = 0; i < op.children.size(); i++) {
			values.push_back(arg(i));
		}
		return "coalesce(" + StringUtil::Join(values, ", ") + ")";
	}
	default:
		ThrowUntranslatable(op);
	}
}

string ClickhouseExpression::Translate(const Expression &expr, const std::function<string(idx_t)> &resolve) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_REF:
		return resolve(expr.Cast<BoundReferenceExpression>().index);
	case ExpressionClass::BOUND_PARAMETER:
		// a parameter whose value is not known yet (PREPARE): when one is supplied, DuckDB's binder emits it as a
		// constant instead (ExpressionBinder::BindExpression(ParameterExpression &)). ClickhouseDml does not
		// translate plans holding parameters (see ClickhouseDmlStatement::unbound_parameters)
		throw NotImplementedException("prepared-statement parameter %s has no value yet", expr.ToString());
	case ExpressionClass::BOUND_CONSTANT:
		try {
			return Literal(expr.Cast<BoundConstantExpression>().value);
		} catch (NotImplementedException &) {
			ThrowUntranslatable(expr);
		}
	case ExpressionClass::BOUND_COMPARISON: {
		auto &comparison = expr.Cast<BoundComparisonExpression>();
		// a comparison with a NULL constant is what DuckDB's optimizer leaves of an IN / NOT IN list holding NULL
		// (x NOT IN (1, NULL) becomes x != 1 AND x != NULL): rejected like the list itself (see COMPARE_IN).
		// Nothing else produces one: DuckDB folds a hand-written x = NULL to NULL
		if (IsNullConstant(*comparison.left) || IsNullConstant(*comparison.right)) {
			ThrowUntranslatable(expr);
		}
		return "(" + Translate(*comparison.left, resolve) + " " + ComparisonOperator(expr) + " " +
		       Translate(*comparison.right, resolve) + ")";
	}
	case ExpressionClass::BOUND_CONJUNCTION: {
		auto &conjunction = expr.Cast<BoundConjunctionExpression>();
		if (conjunction.children.empty()) {
			ThrowUntranslatable(expr);
		}
		vector<string> parts;
		for (auto &child : conjunction.children) {
			parts.push_back(Translate(*child, resolve));
		}
		string separator;
		switch (expr.GetExpressionType()) {
		case ExpressionType::CONJUNCTION_AND:
			separator = " AND ";
			break;
		case ExpressionType::CONJUNCTION_OR:
			separator = " OR ";
			break;
		default:
			ThrowUntranslatable(expr);
		}
		return "(" + StringUtil::Join(parts, separator) + ")";
	}
	case ExpressionClass::BOUND_OPERATOR:
		return TranslateOperator(expr.Cast<BoundOperatorExpression>(), resolve);
	case ExpressionClass::BOUND_BETWEEN: {
		auto &between = expr.Cast<BoundBetweenExpression>();
		auto input = Translate(*between.input, resolve);
		return "(" + input + (between.lower_inclusive ? " >= " : " > ") + Translate(*between.lower, resolve) +
		       " AND " + input + (between.upper_inclusive ? " <= " : " < ") + Translate(*between.upper, resolve) +
		       ")";
	}
	case ExpressionClass::BOUND_FUNCTION:
		return TranslateFunction(expr.Cast<BoundFunctionExpression>(), resolve);
	case ExpressionClass::BOUND_CAST: {
		auto &cast = expr.Cast<BoundCastExpression>();
		if (cast.try_cast) {
			ThrowUntranslatable(expr);
		}
		auto &source_type = cast.child->return_type;
		if (source_type != cast.return_type &&
		    (IsTimeZoneDependent(source_type) || IsTimeZoneDependent(cast.return_type))) {
			// DuckDB converts with the session's TimeZone setting, ClickHouse with the column's or the server's
			// time zone
			throw NotImplementedException("cast from %s to %s depends on the time zone: %s", source_type.ToString(),
			                              cast.return_type.ToString(), expr.ToString());
		}
		string type;
		try {
			type = ClickhouseDdlTypes::ToClickhouse(cast.return_type, true);
		} catch (NotImplementedException &) {
			ThrowUntranslatable(expr);
		}
		return "CAST(" + Translate(*cast.child, resolve) + " AS " + type + ")";
	}
	case ExpressionClass::BOUND_CASE: {
		auto &case_expr = expr.Cast<BoundCaseExpression>();
		string sql = "(CASE";
		for (auto &check : case_expr.case_checks) {
			sql += " WHEN " + Translate(*check.when_expr, resolve) + " THEN " + Translate(*check.then_expr, resolve);
		}
		return sql + " ELSE " + Translate(*case_expr.else_expr, resolve) + " END)";
	}
	default:
		ThrowUntranslatable(expr);
	}
}

string ClickhouseExpression::TranslateTableFilter(const TableFilter &filter, const LogicalType &column_type,
                                                  const string &column) {
	switch (filter.filter_type) {
	case TableFilterType::OPTIONAL_FILTER:
		return string();
	case TableFilterType::DYNAMIC_FILTER:
	case TableFilterType::BLOOM_FILTER:
		throw NotImplementedException("runtime filter %s", EnumUtil::ToString(filter.filter_type));
	case TableFilterType::CONJUNCTION_AND: {
		vector<string> parts;
		for (auto &child : filter.Cast<ConjunctionAndFilter>().child_filters) {
			auto part = TranslateTableFilter(*child, column_type, column);
			if (!part.empty()) {
				parts.push_back(part);
			}
		}
		return parts.empty() ? string() : "(" + StringUtil::Join(parts, " AND ") + ")";
	}
	case TableFilterType::CONJUNCTION_OR: {
		vector<string> parts;
		for (auto &child : filter.Cast<ConjunctionOrFilter>().child_filters) {
			auto part = TranslateTableFilter(*child, column_type, column);
			if (part.empty()) {
				// dropping a branch of an OR would narrow it; dropping the whole OR would widen the statement
				throw NotImplementedException("OR filter with a non-exact branch");
			}
			parts.push_back(part);
		}
		if (parts.empty()) {
			throw NotImplementedException("empty OR filter");
		}
		return "(" + StringUtil::Join(parts, " OR ") + ")";
	}
	default: {
		// every other filter (constant comparison, IS [NOT] NULL, IN list, expression, struct extract) as the
		// expression DuckDB itself would evaluate for it, through the same allow-list as a LogicalFilter's
		BoundReferenceExpression column_ref(column_type, 0);
		auto expr = filter.ToExpression(column_ref);
		return Translate(*expr, [&](idx_t index) -> string {
			if (index != 0) {
				throw NotImplementedException("filter referencing another column");
			}
			return column;
		});
	}
	}
}

} // namespace duckdb
