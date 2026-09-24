#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

//! A parsed ClickHouse type, e.g. Nullable(DateTime64(3, 'UTC'))
struct ClickhouseTypeNode {
	//! Type name, e.g. "Nullable", "DateTime64", "Int32"
	string name;
	//! Arguments that are types (Array(T), Tuple(...), Map(K, V), Nullable(T), ...)
	vector<ClickhouseTypeNode> children;
	//! Element names of named Tuple/Nested arguments (same length as children, empty when unnamed)
	vector<string> field_names;
	//! Literal arguments, verbatim: numbers, quoted strings and enum entries like 'a' = 1
	vector<string> literals;
	//! The (trimmed) text this node was parsed from
	string text;
};

class ClickhouseTypeParser {
public:
	static ClickhouseTypeNode Parse(const string &type_text);
};

struct ClickhouseColumnType {
	LogicalType type;
	//! False when values cannot be read at all (AggregateFunction states)
	bool readable = true;
};

class ClickhouseTypes {
public:
	static ClickhouseColumnType ToDuckDB(const ClickhouseTypeNode &node);
	//! ClickHouse expression that reads `expr` in a form the conversion code decodes; returns `expr` itself for
	//! natively decoded types
	static string ReadExpression(const ClickhouseTypeNode &node, const string &expr);
	//! Whether filters and ORDER BY on a column of this type may be evaluated by ClickHouse
	static bool SupportsPushdown(const ClickhouseTypeNode &node);
	static bool IsNullable(const ClickhouseTypeNode &node);
};

//! A column of a ClickHouse table or query result
struct ClickhouseColumnInfo {
	string name;
	//! Type as reported by ClickHouse, e.g. Nullable(DateTime64(3, 'UTC'))
	string clickhouse_type;
	ClickhouseTypeNode type_node;
	LogicalType type;
	bool readable = true;
	//! system.columns.default_kind for table columns: "", DEFAULT, MATERIALIZED or ALIAS (EPHEMERAL columns are not
	//! loaded). Empty for query results
	string default_kind;

	static ClickhouseColumnInfo Create(const string &name, const string &clickhouse_type);
};

} // namespace duckdb
