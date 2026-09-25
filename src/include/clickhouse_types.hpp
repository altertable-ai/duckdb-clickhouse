#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_idx.hpp"
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

//! A column type's LowCardinality and Nullable wrappers and the type inside them: LowCardinality(Nullable(String))
//! is {low_cardinality, nullable, String}
struct ClickhouseTypeWrappers {
	bool low_cardinality = false;
	bool nullable = false;
	const ClickhouseTypeNode &base;

	static ClickhouseTypeWrappers Of(const ClickhouseTypeNode &node);
	//! `type` in the same wrappers
	string Wrap(const string &type) const;
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
	//! Whether ClickHouse compares values of this type exactly as DuckDB reads them, which UPDATE/DELETE predicates
	//! rely on. False (mirroring SupportsPushdown's exclusions) for a type holding, at any nesting level:
	//! - DateTime64 with a precision above 6, which DuckDB reads floor-truncated to microseconds;
	//! - FixedString, whose NUL padding DuckDB sees but ClickHouse's `fs = 'ab'` ignores
	static bool IsComparedExactly(const ClickhouseTypeNode &node);
	static bool IsNullable(const ClickhouseTypeNode &node);
	//! Digits of a second of a DateTime (0) or DateTime64 (Nullable/LowCardinality unwrapped); invalid otherwise
	static optional_idx DateTimePrecision(const ClickhouseTypeNode &node);
	//! Scale of a Decimal, Decimal32/64/128/256 type (not unwrapped); invalid for any other type
	static optional_idx DecimalScale(const ClickhouseTypeNode &node);
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
