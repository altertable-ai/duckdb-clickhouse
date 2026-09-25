#include "clickhouse_expression.hpp"

#include "clickhouse_ddl_types.hpp"
#include "clickhouse_filter_pushdown.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/error_data.hpp"
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
		// naive timestamps are stored as UTC (see ClickhouseDdlTypes); seconds and milliseconds widen to
		// microseconds exactly
		return ClickhouseFilterPushdown::TransformConstant(
		    Value::TIMESTAMPTZ(timestamp_tz_t(value.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>())));
	case LogicalTypeId::TIMESTAMP_NS: {
		// every nanosecond: a cast to TIMESTAMP would drop the sub-microsecond part
		auto timestamp = TimestampNSValue::Get(value);
		if (!Value::IsFinite(timestamp)) {
			throw NotImplementedException("infinite timestamps have no ClickHouse literal");
		}
		return StringUtil::Format("fromUnixTimestamp64Nano(%d, 'UTC')", timestamp.value);
	}
	default:
		return ClickhouseFilterPushdown::TransformConstant(value);
	}
}

string ClickhouseExpression::InList(const string &left, const vector<string> &values, bool negated) {
	return "if(isNull(" + left + "), NULL, (" + left + (negated ? " NOT IN (" : " IN (") +
	       StringUtil::Join(values, ", ") + ")))";
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

//! Types whose VARCHAR form is the same text in DuckDB and ClickHouse (CAST(x AS String)). Any other cast to VARCHAR
//! is rejected, whether written or implicit (e.g. the operands of ||, LIKE, contains): a DOUBLE, DECIMAL, TIMESTAMP,
//! ... would be formatted differently (e.g. DuckDB's 1.0 is ClickHouse's 1)
static bool HasSameTextForm(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::DATE:
		return true;
	default:
		return false;
	}
}

//! How a cast whose ClickHouse CAST would not match DuckDB's result must round its input first
enum class CastRounding : uint8_t {
	//! A plain CAST gives DuckDB's result
	NONE,
	//! roundBankers(x): DuckDB converts FLOAT/DOUBLE to integers with std::nearbyint (half to even), ClickHouse's
	//! CAST truncates
	HALF_TO_EVEN,
	//! round(x, <target scale>): DuckDB rounds DECIMAL to integers and to DECIMALs with a smaller scale half away
	//! from zero, ClickHouse's CAST truncates; ClickHouse's round() on a Decimal rounds half away from zero
	HALF_AWAY_FROM_ZERO
};

static bool IsTimestamp(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
		return true;
	default:
		return false;
	}
}

//! Decimal digits of a second a (naive) timestamp type holds
static idx_t TimestampDigits(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TIMESTAMP_SEC:
		return 0;
	case LogicalTypeId::TIMESTAMP_MS:
		return 3;
	case LogicalTypeId::TIMESTAMP_NS:
		return 9;
	default:
		return 6;
	}
}

[[noreturn]] static void ThrowInexactCast(const LogicalType &source, const LogicalType &target, const string &why) {
	throw NotImplementedException("cast from %s to %s %s", source.ToString(), target.ToString(), why);
}

//! Whether (and how) a cast from `source` to `target` translates into a ClickHouse CAST with DuckDB's result.
//! Throws NotImplementedException for every cast that does not (see the "as built" list in the design spec, §6):
//! only casts listed here translate. A VARCHAR source follows ClickHouse's parse rules, which accept and refuse
//! other strings than DuckDB's (ClickHouse fails on CAST('2.5' AS Int32) instead of rounding), but only for
//! targets whose parse does not round
static CastRounding ClassifyCast(const LogicalType &source, const LogicalType &target) {
	if (source == target || source.id() == LogicalTypeId::SQLNULL) {
		return CastRounding::NONE;
	}
	if (IsTimeZoneDependent(source) || IsTimeZoneDependent(target)) {
		// DuckDB converts with the session's TimeZone setting, ClickHouse with the column's or the server's
		// time zone
		ThrowInexactCast(source, target, "depends on the time zone");
	}
	if (target.IsJSONType()) {
		// ClickHouse parses the text into its JSON object type
		ThrowInexactCast(source, target, "has no exact ClickHouse translation");
	}
	if (target.id() == LogicalTypeId::VARCHAR) {
		// a JSON value is its text in both (a JSON column is read through toJSONString())
		if (!HasSameTextForm(source)) {
			throw NotImplementedException("cast from %s to VARCHAR, which ClickHouse formats differently",
			                              source.ToString());
		}
		return CastRounding::NONE;
	}
	if (source.IsJSONType()) {
		ThrowInexactCast(source, target, "has no exact ClickHouse translation");
	}
	auto source_id = source.id();
	auto is_varchar = source_id == LogicalTypeId::VARCHAR;
	switch (target.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		if (source.IsIntegral() || source_id == LogicalTypeId::BOOLEAN || is_varchar) {
			return CastRounding::NONE;
		}
		if (source.IsFloating()) {
			return CastRounding::HALF_TO_EVEN;
		}
		if (source_id == LogicalTypeId::DECIMAL) {
			return CastRounding::HALF_AWAY_FROM_ZERO;
		}
		break;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		if (source.IsFloating()) {
			// both round to nearest (the C++ conversion)
			return CastRounding::NONE;
		}
		if (source.IsIntegral() && source_id != LogicalTypeId::HUGEINT && source_id != LogicalTypeId::UHUGEINT) {
			// a single conversion in both; DuckDB converts 128-bit integers in two steps
			return CastRounding::NONE;
		}
		if (source_id == LogicalTypeId::DECIMAL &&
		    DecimalType::GetWidth(source) <= (target.id() == LogicalTypeId::FLOAT ? 7 : 15)) {
			// unscaled value / 10^scale in both, in the target type, while the unscaled value converts exactly
			// (below 2^24 / 2^53); DuckDB computes wider values differently (TryCastDecimalToFloatingPoint)
			return CastRounding::NONE;
		}
		// VARCHAR: ClickHouse's parse is not correctly rounded (CAST('1.7091' AS Float64) = 1.7090999999999998),
		// and precise_float_parsing does not reach a mutation
		break;
	case LogicalTypeId::DECIMAL:
		if (source.IsIntegral()) {
			return CastRounding::NONE;
		}
		if (source_id == LogicalTypeId::DECIMAL) {
			return DecimalType::GetScale(source) > DecimalType::GetScale(target) ? CastRounding::HALF_AWAY_FROM_ZERO
			                                                                     : CastRounding::NONE;
		}
		// FLOAT/DOUBLE and VARCHAR: DuckDB rounds to the scale, ClickHouse truncates
		break;
	case LogicalTypeId::BOOLEAN:
		if (source.IsIntegral() || is_varchar) {
			return CastRounding::NONE;
		}
		break;
	case LogicalTypeId::DATE:
		if (is_varchar || IsTimestamp(source)) {
			// both floor a timestamp to its day
			return CastRounding::NONE;
		}
		break;
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
		if (source_id == LogicalTypeId::DATE ||
		    (IsTimestamp(source) && TimestampDigits(source) <= TimestampDigits(target))) {
			return CastRounding::NONE;
		}
		if (is_varchar && (target.id() == LogicalTypeId::TIMESTAMP || target.id() == LogicalTypeId::TIMESTAMP_SEC)) {
			// both truncate extra digits of a microsecond timestamp; ClickHouse refuses any fraction for seconds
			return CastRounding::NONE;
		}
		// fewer digits: DuckDB rounds (truncates from nanoseconds), ClickHouse truncates toward zero
		break;
	case LogicalTypeId::TIME_NS:
		if (source_id == LogicalTypeId::TIME) {
			return CastRounding::NONE;
		}
		break;
	case LogicalTypeId::UUID:
		if (is_varchar) {
			return CastRounding::NONE;
		}
		break;
	case LogicalTypeId::ENUM:
		if (is_varchar || source_id == LogicalTypeId::ENUM) {
			// by label in both
			return CastRounding::NONE;
		}
		break;
	case LogicalTypeId::LIST:
		if (source_id == LogicalTypeId::LIST &&
		    ClassifyCast(ListType::GetChildType(source), ListType::GetChildType(target)) == CastRounding::NONE) {
			return CastRounding::NONE;
		}
		break;
	case LogicalTypeId::MAP:
		if (source_id == LogicalTypeId::MAP &&
		    ClassifyCast(MapType::KeyType(source), MapType::KeyType(target)) == CastRounding::NONE &&
		    ClassifyCast(MapType::ValueType(source), MapType::ValueType(target)) == CastRounding::NONE) {
			return CastRounding::NONE;
		}
		break;
	case LogicalTypeId::STRUCT: {
		if (source_id != LogicalTypeId::STRUCT) {
			break;
		}
		auto &source_children = StructType::GetChildTypes(source);
		auto &target_children = StructType::GetChildTypes(target);
		if (source_children.size() != target_children.size()) {
			break;
		}
		bool exact = true;
		for (idx_t i = 0; i < source_children.size() && exact; i++) {
			exact = source_children[i].first == target_children[i].first &&
			        ClassifyCast(source_children[i].second, target_children[i].second) == CastRounding::NONE;
		}
		if (exact) {
			return CastRounding::NONE;
		}
		break;
	}
	default:
		break;
	}
	ThrowInexactCast(source, target, "has no exact ClickHouse translation");
}

static string CastSql(CastRounding rounding, const string &sql, const LogicalType &target,
                      const string &clickhouse_type) {
	switch (rounding) {
	case CastRounding::HALF_TO_EVEN:
		return "CAST(roundBankers(" + sql + ") AS " + clickhouse_type + ")";
	case CastRounding::HALF_AWAY_FROM_ZERO: {
		auto scale = target.id() == LogicalTypeId::DECIMAL ? DecimalType::GetScale(target) : 0;
		return "CAST(round(" + sql + ", " + to_string(scale) + ") AS " + clickhouse_type + ")";
	}
	default:
		return "CAST(" + sql + " AS " + clickhouse_type + ")";
	}
}

string ClickhouseExpression::Cast(const string &sql, const LogicalType &source, const LogicalType &target,
                                  const string &clickhouse_type) {
	return CastSql(ClassifyCast(source, target), sql, target, clickhouse_type);
}

void ClickhouseExpression::CheckPlainCast(const LogicalType &source, const LogicalType &target) {
	if (ClassifyCast(source, target) != CastRounding::NONE) {
		throw NotImplementedException("DuckDB rounds this cast, ClickHouse's CAST truncates");
	}
}

//! DuckDB computes FLOAT arithmetic in single precision, ClickHouse promotes Float32 to Float64: rounding the
//! Float64 result of + - * / (or of a negation) of two Float32 values to Float32 gives the single-precision result
static string ArithmeticResult(const BoundFunctionExpression &function, const string &sql) {
	return function.return_type.id() == LogicalTypeId::FLOAT ? "toFloat32(" + sql + ")" : sql;
}

//! Whether DuckDB's arithmetic function `function` computes what ClickHouse's operator does (overflow and division
//! by zero aside): numbers only, or a DATE plus/minus a number of days
static bool IsTranslatableArithmetic(const BoundFunctionExpression &function) {
	auto &name = function.function.name;
	auto &children = function.children;
	auto all_numeric = function.return_type.IsNumeric();
	for (auto &child : children) {
		all_numeric = all_numeric && child->return_type.IsNumeric();
	}
	if (all_numeric) {
		return true;
	}
	if (function.return_type.id() != LogicalTypeId::DATE || children.size() != 2) {
		return false;
	}
	auto &left = children[0]->return_type;
	auto &right = children[1]->return_type;
	if (name == "+") {
		return (left.id() == LogicalTypeId::DATE && right.IsIntegral()) ||
		       (left.IsIntegral() && right.id() == LogicalTypeId::DATE);
	}
	return name == "-" && left.id() == LogicalTypeId::DATE && right.IsIntegral();
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
	auto is_arithmetic = name == "+" || name == "-" || name == "*" || name == "/" || name == "//" || name == "%";
	if (is_arithmetic && !IsTranslatableArithmetic(function)) {
		throw NotImplementedException("%s on %s has no exact ClickHouse translation: %s", name,
		                              function.return_type.ToString(), function.ToString());
	}
	auto integral_result = function.return_type.IsIntegral();
	if (children.size() == 2 && (name == "+" || name == "-" || name == "*")) {
		return ArithmeticResult(function, "(" + arg(0) + " " + name + " " + arg(1) + ")");
	}
	if (children.size() == 1 && name == "-") {
		// negate(), not a leading "-": "-" followed by a negative literal would start a "--" comment
		return ArithmeticResult(function, "negate(" + arg(0) + ")");
	}
	if (children.size() == 2 && (name == "//" || name == "/") && integral_result) {
		// integer division (DuckDB's "/" is one too with SET integer_division = true); truncates toward zero in
		// both
		return "intDiv(" + arg(0) + ", " + arg(1) + ")";
	}
	if (children.size() == 2 && name == "/" && function.return_type.IsFloating()) {
		return ArithmeticResult(function, "(" + arg(0) + " / " + arg(1) + ")");
	}
	if (children.size() == 2 && name == "%" && integral_result) {
		return "(" + arg(0) + " % " + arg(1) + ")";
	}
	if (is_arithmetic) {
		// DuckDB's // on FLOAT/DOUBLE (and DECIMAL, which it casts to DOUBLE) is a plain division; % on them is
		// fmod(), which ClickHouse does not compute the same way for large values (1e20 % 3)
		throw NotImplementedException("%s on %s has no exact ClickHouse translation: %s", name,
		                              function.return_type.ToString(), function.ToString());
	}
	if (children.size() == 2 && (name == "~~" || name == "!~~" || name == "~~*" || name == "!~~*")) {
		auto negated = name[0] == '!';
		auto keyword = StringUtil::EndsWith(name, "*") ? "ILIKE" : "LIKE";
		return "(" + arg(0) + (negated ? " NOT " : " ") + keyword + " " + LikePattern(*children[1]) + ")";
	}
	if (children.size() == 2 && name == "||" && is_string(0) && is_string(1)) {
		// NULL if either side is NULL, in both (unlike DuckDB's concat(), which skips NULLs). DuckDB's binder casts a
		// non-VARCHAR operand to VARCHAR implicitly (BindConcatOperator): that cast is checked like any other, see
		// HasSameTextForm
		return "concat(" + arg(0) + ", " + arg(1) + ")";
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
		return ClickhouseExpression::InList(arg(0), values, op.GetExpressionType() == ExpressionType::COMPARE_NOT_IN);
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
		CastRounding rounding;
		try {
			rounding = ClassifyCast(cast.child->return_type, cast.return_type);
		} catch (NotImplementedException &ex) {
			throw NotImplementedException("%s: %s", ErrorData(ex).RawMessage(), expr.ToString());
		}
		string type;
		try {
			type = ClickhouseDdlTypes::ToClickhouse(cast.return_type, true);
		} catch (NotImplementedException &) {
			ThrowUntranslatable(expr);
		}
		return CastSql(rounding, Translate(*cast.child, resolve), cast.return_type, type);
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
