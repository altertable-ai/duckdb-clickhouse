#include "clickhouse_types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unordered_set.hpp"

#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Parsing
//===--------------------------------------------------------------------===//
static string TrimCopy(const string &text) {
	string result = text;
	StringUtil::Trim(result);
	return result;
}

//! Splits an argument list on top-level commas, respecting parentheses, quotes and backticks
static vector<string> SplitArguments(const string &text, const string &full_type) {
	vector<string> result;
	idx_t depth = 0;
	char quote = '\0';
	string current;
	for (idx_t i = 0; i < text.size(); i++) {
		char c = text[i];
		if (quote != '\0') {
			current += c;
			if (c == '\\' && i + 1 < text.size()) {
				current += text[++i];
			} else if (c == quote) {
				quote = '\0';
			}
			continue;
		}
		if (c == '\'' || c == '`' || c == '"') {
			quote = c;
			current += c;
		} else if (c == '(') {
			depth++;
			current += c;
		} else if (c == ')') {
			if (depth == 0) {
				throw InvalidInputException("Malformed ClickHouse type \"%s\"", full_type);
			}
			depth--;
			current += c;
		} else if (c == ',' && depth == 0) {
			result.push_back(TrimCopy(current));
			current.clear();
		} else {
			current += c;
		}
	}
	if (depth != 0 || quote != '\0') {
		throw InvalidInputException("Malformed ClickHouse type \"%s\"", full_type);
	}
	result.push_back(TrimCopy(current));
	return result;
}

static bool IsLiteralArgument(const string &argument) {
	auto c = argument[0];
	return c == '\'' || c == '-' || c == '+' || StringUtil::CharacterIsDigit(c);
}

//! Splits "name Type" (a named Tuple / Nested element) into name and type. The name stays empty when there is none.
static void SplitFieldName(const string &argument, string &field_name, string &type_text) {
	field_name.clear();
	type_text = argument;
	if (argument[0] == '`' || argument[0] == '"') {
		auto quote = argument[0];
		string name;
		idx_t i = 1;
		for (; i < argument.size(); i++) {
			if (argument[i] == '\\' && i + 1 < argument.size()) {
				name += argument[++i];
			} else if (argument[i] == quote) {
				break;
			} else {
				name += argument[i];
			}
		}
		auto rest = i + 1 < argument.size() ? TrimCopy(argument.substr(i + 1)) : string();
		if (!rest.empty()) {
			field_name = name;
			type_text = rest;
		}
		return;
	}
	for (idx_t i = 0; i < argument.size(); i++) {
		if (argument[i] == '(') {
			return;
		}
		if (StringUtil::CharacterIsSpace(argument[i])) {
			auto rest = TrimCopy(argument.substr(i + 1));
			if (!rest.empty()) {
				field_name = argument.substr(0, i);
				type_text = rest;
			}
			return;
		}
	}
}

//! Maximum number of nested type levels the parser will recurse into. Guards against a stack overflow on
//! adversarial input like Array(Array(Array(...))); everything downstream (ToDuckDB, ReadExpression, ...)
//! only ever walks trees built by the parser, so bounding recursion here is enough to bound it everywhere.
static constexpr idx_t MAX_TYPE_NESTING_DEPTH = 64;

static ClickhouseTypeNode ParseInternal(const string &type_text, idx_t depth) {
	if (depth > MAX_TYPE_NESTING_DEPTH) {
		throw InvalidInputException("ClickHouse type \"%s\" is nested too deeply", type_text);
	}
	ClickhouseTypeNode node;
	node.text = TrimCopy(type_text);
	if (node.text.empty()) {
		throw InvalidInputException("Malformed ClickHouse type \"%s\"", type_text);
	}
	auto paren = node.text.find('(');
	if (paren == string::npos) {
		node.name = node.text;
		return node;
	}
	if (node.text.back() != ')') {
		throw InvalidInputException("Malformed ClickHouse type \"%s\"", type_text);
	}
	node.name = TrimCopy(node.text.substr(0, paren));
	auto inner = node.text.substr(paren + 1, node.text.size() - paren - 2);
	if (TrimCopy(inner).empty()) {
		return node;
	}
	for (auto &argument : SplitArguments(inner, type_text)) {
		if (argument.empty()) {
			throw InvalidInputException("Malformed ClickHouse type \"%s\"", type_text);
		}
		if (IsLiteralArgument(argument)) {
			node.literals.push_back(argument);
			continue;
		}
		string field_name;
		string child_text;
		SplitFieldName(argument, field_name, child_text);
		node.children.push_back(ParseInternal(child_text, depth + 1));
		node.field_names.push_back(field_name);
	}
	return node;
}

ClickhouseTypeNode ClickhouseTypeParser::Parse(const string &type_text) {
	return ParseInternal(type_text, 0);
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
//! Strips wrappers that do not change how values are decoded
static const ClickhouseTypeNode &Unwrap(const ClickhouseTypeNode &node) {
	if ((node.name == "Nullable" || node.name == "LowCardinality") && node.children.size() == 1) {
		return Unwrap(node.children[0]);
	}
	if (node.name == "SimpleAggregateFunction" && !node.children.empty()) {
		return Unwrap(node.children.back());
	}
	return node;
}

static int64_t ParseIntegerLiteral(const ClickhouseTypeNode &node, idx_t index, int64_t default_value) {
	if (index >= node.literals.size()) {
		return default_value;
	}
	try {
		return std::stoll(node.literals[index]);
	} catch (std::exception &) {
		throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
	}
}

static bool GetDecimalInfo(const ClickhouseTypeNode &node, idx_t &width, idx_t &scale) {
	int64_t raw_width;
	int64_t raw_scale;
	if (node.name == "Decimal") {
		raw_width = ParseIntegerLiteral(node, 0, 10);
		raw_scale = ParseIntegerLiteral(node, 1, 0);
	} else if (node.name == "Decimal32") {
		raw_width = 9;
		raw_scale = ParseIntegerLiteral(node, 0, 0);
	} else if (node.name == "Decimal64") {
		raw_width = 18;
		raw_scale = ParseIntegerLiteral(node, 0, 0);
	} else if (node.name == "Decimal128") {
		raw_width = 38;
		raw_scale = ParseIntegerLiteral(node, 0, 0);
	} else if (node.name == "Decimal256") {
		raw_width = 76;
		raw_scale = ParseIntegerLiteral(node, 0, 0);
	} else {
		return false;
	}
	// ClickHouse requires 1 <= width <= 76 and 0 <= scale <= width; reject anything outside that range
	// before it reaches NumericCast, which would otherwise raise an InternalException on overflow/underflow.
	if (raw_width < 1 || raw_width > 76 || raw_scale < 0 || raw_scale > raw_width) {
		throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
	}
	width = NumericCast<idx_t>(raw_width);
	scale = NumericCast<idx_t>(raw_scale);
	return true;
}

struct EnumEntry {
	string label;
	int64_t value;
};

//! Parses entries like 'label' = 5 and returns them ordered by value
static vector<EnumEntry> ParseEnumEntries(const ClickhouseTypeNode &node) {
	vector<EnumEntry> entries;
	for (idx_t i = 0; i < node.literals.size(); i++) {
		auto &literal = node.literals[i];
		if (literal.empty() || literal[0] != '\'') {
			throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
		}
		string label;
		bool closed = false;
		idx_t pos = 1;
		for (; pos < literal.size(); pos++) {
			char c = literal[pos];
			if (c == '\\' && pos + 1 < literal.size()) {
				char escaped = literal[++pos];
				switch (escaped) {
				case 'n':
					label += '\n';
					break;
				case 't':
					label += '\t';
					break;
				case 'r':
					label += '\r';
					break;
				case '0':
					label += '\0';
					break;
				default:
					label += escaped;
				}
			} else if (c == '\'') {
				closed = true;
				pos++;
				break;
			} else {
				label += c;
			}
		}
		if (!closed) {
			throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
		}
		auto rest = TrimCopy(literal.substr(pos));
		int64_t value = NumericCast<int64_t>(i + 1);
		if (!rest.empty()) {
			if (rest[0] != '=') {
				throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
			}
			try {
				value = std::stoll(TrimCopy(rest.substr(1)));
			} catch (std::exception &) {
				throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
			}
		}
		entries.push_back({label, value});
	}
	std::stable_sort(entries.begin(), entries.end(),
	                 [](const EnumEntry &a, const EnumEntry &b) { return a.value < b.value; });
	return entries;
}

static bool IsSemiStructured(const string &name) {
	return name == "JSON" || name == "Object" || name == "Variant" || name == "Dynamic";
}

static bool IsGeoType(const string &name) {
	return name == "Point" || name == "Ring" || name == "LineString" || name == "MultiLineString" ||
	       name == "Polygon" || name == "MultiPolygon" || name == "Geometry";
}

//! Scalar types that the conversion code decodes straight from the native protocol
static bool IsNativeScalar(const string &name) {
	static const unordered_set<string> NATIVE_TYPES = {
	    "Bool",   "Int8",   "Int16",       "Int32", "Int64",      "UInt8",  "UInt16", "UInt32",
	    "UInt64", "Int128", "UInt128",     "Float32", "Float64",  "String", "FixedString", "Date",
	    "Date32", "DateTime", "DateTime64", "Time",  "Time64",     "UUID",   "Enum8",  "Enum16",
	    "Nothing", "AggregateFunction"};
	return NATIVE_TYPES.find(name) != NATIVE_TYPES.end();
}

//===--------------------------------------------------------------------===//
// ToDuckDB
//===--------------------------------------------------------------------===//
ClickhouseColumnType ClickhouseTypes::ToDuckDB(const ClickhouseTypeNode &node) {
	auto &type = Unwrap(node);
	auto &name = type.name;
	ClickhouseColumnType result;
	result.type = LogicalType::VARCHAR;
	idx_t width;
	idx_t scale;
	if (name == "Bool") {
		result.type = LogicalType::BOOLEAN;
	} else if (name == "Int8") {
		result.type = LogicalType::TINYINT;
	} else if (name == "Int16") {
		result.type = LogicalType::SMALLINT;
	} else if (name == "Int32") {
		result.type = LogicalType::INTEGER;
	} else if (name == "Int64") {
		result.type = LogicalType::BIGINT;
	} else if (name == "UInt8") {
		result.type = LogicalType::UTINYINT;
	} else if (name == "UInt16") {
		result.type = LogicalType::USMALLINT;
	} else if (name == "UInt32") {
		result.type = LogicalType::UINTEGER;
	} else if (name == "UInt64") {
		result.type = LogicalType::UBIGINT;
	} else if (name == "Int128") {
		result.type = LogicalType::HUGEINT;
	} else if (name == "UInt128") {
		result.type = LogicalType::UHUGEINT;
	} else if (name == "Float32" || name == "BFloat16") {
		result.type = LogicalType::FLOAT;
	} else if (name == "Float64") {
		result.type = LogicalType::DOUBLE;
	} else if (GetDecimalInfo(type, width, scale)) {
		if (width <= 38) {
			result.type = LogicalType::DECIMAL(NumericCast<uint8_t>(width), NumericCast<uint8_t>(scale));
		}
	} else if (name == "String" || name == "FixedString") {
		result.type = LogicalType::VARCHAR;
	} else if (name == "Date" || name == "Date32") {
		result.type = LogicalType::DATE;
	} else if (name == "DateTime" || name == "DateTime64") {
		result.type = LogicalType::TIMESTAMP_TZ;
	} else if (name == "Time") {
		result.type = LogicalType::TIME;
	} else if (name == "Time64") {
		result.type = ParseIntegerLiteral(type, 0, 3) > 6 ? LogicalType::TIME_NS : LogicalType::TIME;
	} else if (name == "UUID") {
		result.type = LogicalType::UUID;
	} else if ((name == "Enum8" || name == "Enum16") && !type.literals.empty()) {
		auto entries = ParseEnumEntries(type);
		Vector labels(LogicalType::VARCHAR, entries.size());
		auto label_data = FlatVector::GetData<string_t>(labels);
		for (idx_t i = 0; i < entries.size(); i++) {
			label_data[i] = StringVector::AddString(labels, entries[i].label);
		}
		result.type = LogicalType::ENUM(labels, entries.size());
	} else if (name == "Array" && type.children.size() == 1) {
		auto child = ToDuckDB(type.children[0]);
		result.type = LogicalType::LIST(child.type);
		result.readable = child.readable;
	} else if (name == "Tuple" && !type.children.empty()) {
		child_list_t<LogicalType> children;
		for (idx_t i = 0; i < type.children.size(); i++) {
			auto child = ToDuckDB(type.children[i]);
			result.readable = result.readable && child.readable;
			auto field_name = type.field_names[i].empty() ? to_string(i + 1) : type.field_names[i];
			children.emplace_back(field_name, child.type);
		}
		result.type = LogicalType::STRUCT(std::move(children));
	} else if (name == "Map" && type.children.size() == 2) {
		auto key = ToDuckDB(type.children[0]);
		auto value = ToDuckDB(type.children[1]);
		result.type = LogicalType::MAP(key.type, value.type);
		result.readable = key.readable && value.readable;
	} else if (IsSemiStructured(name)) {
		result.type = LogicalType::JSON();
	} else if (name == "AggregateFunction") {
		result.readable = false;
	}
	// everything else (Nothing, IPv4/IPv6, (U)Int256, Decimal256, geo types, intervals, ...) is VARCHAR
	return result;
}

//===--------------------------------------------------------------------===//
// Read expressions
//===--------------------------------------------------------------------===//
static string ReadExpressionInternal(const ClickhouseTypeNode &node, const string &expr, idx_t depth);

//! Applies the element read expression to every element of an array expression
static string ArrayReadExpression(const ClickhouseTypeNode &element, const string &array_expr, idx_t depth) {
	auto variable = "x" + to_string(depth);
	auto element_expr = ReadExpressionInternal(element, variable, depth + 1);
	if (element_expr == variable) {
		return array_expr;
	}
	return "arrayMap(" + variable + " -> " + element_expr + ", " + array_expr + ")";
}

static string ReadExpressionInternal(const ClickhouseTypeNode &node, const string &expr, idx_t depth) {
	auto &type = Unwrap(node);
	auto &name = type.name;
	idx_t width;
	idx_t scale;
	if (GetDecimalInfo(type, width, scale)) {
		return width <= 38 ? expr : "toString(" + expr + ")";
	}
	if (IsNativeScalar(name)) {
		return expr;
	}
	if (name == "BFloat16") {
		return "toFloat32(" + expr + ")";
	}
	if (IsGeoType(name)) {
		return "wkt(" + expr + ")";
	}
	if (IsSemiStructured(name)) {
		return "toJSONString(" + expr + ")";
	}
	if (name == "Array" && type.children.size() == 1) {
		return ArrayReadExpression(type.children[0], expr, depth);
	}
	if (name == "Tuple" && !type.children.empty()) {
		vector<string> elements;
		bool changed = false;
		for (idx_t i = 0; i < type.children.size(); i++) {
			auto element = "tupleElement(" + expr + ", " + to_string(i + 1) + ")";
			auto element_expr = ReadExpressionInternal(type.children[i], element, depth + 1);
			changed = changed || element_expr != element;
			elements.push_back(element_expr);
		}
		if (!changed) {
			return expr;
		}
		return "tuple(" + StringUtil::Join(elements, ", ") + ")";
	}
	if (name == "Map" && type.children.size() == 2) {
		// Map(K, V) is read as Array(Tuple(K, V)): the same layout as a DuckDB MAP
		return "arrayZip(" + ArrayReadExpression(type.children[0], "mapKeys(" + expr + ")", depth) + ", " +
		       ArrayReadExpression(type.children[1], "mapValues(" + expr + ")", depth) + ")";
	}
	return "toString(" + expr + ")";
}

string ClickhouseTypes::ReadExpression(const ClickhouseTypeNode &node, const string &expr) {
	return ReadExpressionInternal(node, expr, 0);
}

bool ClickhouseTypes::SupportsPushdown(const ClickhouseTypeNode &node) {
	static const unordered_set<string> PUSHDOWN_TYPES = {
	    "Bool",   "Int8",   "Int16", "Int32",  "Int64",    "UInt8",      "UInt16", "UInt32",
	    "UInt64", "Int128", "UInt128", "String", "Date",   "Date32",     "DateTime", "DateTime64",
	    "Enum8",  "Enum16"};
	auto &type = Unwrap(node);
	idx_t width;
	idx_t scale;
	if (GetDecimalInfo(type, width, scale)) {
		return width <= 38;
	}
	if (type.name == "DateTime64") {
		// global-context.md: "precision > 6 floor-truncated" on read. A pushed comparison would be evaluated
		// by ClickHouse at the column's own (higher) precision, matching/excluding rows differently than
		// DuckDB's own (truncated, microsecond) value would -- see fix-round-1 item 2. DateTime64() without an
		// explicit precision defaults to 3 in ClickHouse, well within range.
		return ParseIntegerLiteral(type, 0, 3) <= 6;
	}
	return PUSHDOWN_TYPES.find(type.name) != PUSHDOWN_TYPES.end();
}

bool ClickhouseTypes::IsNullable(const ClickhouseTypeNode &node) {
	if (node.name == "Nullable") {
		return true;
	}
	if ((node.name == "LowCardinality" || node.name == "SimpleAggregateFunction") && !node.children.empty()) {
		return IsNullable(node.children.back());
	}
	return false;
}

ClickhouseColumnInfo ClickhouseColumnInfo::Create(const string &name, const string &clickhouse_type) {
	ClickhouseColumnInfo result;
	result.name = name;
	result.clickhouse_type = clickhouse_type;
	result.type_node = ClickhouseTypeParser::Parse(clickhouse_type);
	auto mapped = ClickhouseTypes::ToDuckDB(result.type_node);
	result.type = std::move(mapped.type);
	result.readable = mapped.readable;
	return result;
}

} // namespace duckdb
