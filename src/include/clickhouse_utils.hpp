#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/identifier.hpp"

namespace duckdb {

class ClickhouseUtils {
public:
	//! Quotes an identifier with backticks, escaping backslashes and backticks
	static string QuoteIdentifier(const string &identifier);
	static string QuoteIdentifier(const Identifier &identifier) {
		return QuoteIdentifier(identifier.GetIdentifierName());
	}
	//! `database`.`table`, both quoted with QuoteIdentifier()
	static string QualifiedName(const string &database, const string &table);
	static string QualifiedName(const Identifier &database, const Identifier &table) {
		return QualifiedName(database.GetIdentifierName(), table.GetIdentifierName());
	}
	static string QualifiedName(const Identifier &database, const string &table) {
		return QualifiedName(database.GetIdentifierName(), table);
	}
	static string QualifiedName(const string &database, const Identifier &table) {
		return QualifiedName(database, table.GetIdentifierName());
	}
	//! Quotes a string literal with single quotes, escaping backslashes and single quotes
	static string QuoteLiteral(const string &literal);
	static string QuoteLiteral(const Identifier &literal) {
		return QuoteLiteral(literal.GetIdentifierName());
	}
	//! Returns true if the byte range is well-formed UTF-8
	static bool IsValidUtf8(const char *data, idx_t size);
	//! Removes trailing whitespace and semicolons, which the native protocol does not accept
	static string StripTrailingSemicolons(string sql);
	//! Throws the error for a write that reaches the extension on a database attached with READ_ONLY. Raised
	//! through ClickhouseCatalog::ThrowIfReadOnly() by every write path: the catalog's Plan* hooks (which run
	//! before DuckDB's own read-only check), the INSERT sink (which resolves its database again when it runs) and
	//! clickhouse_execute() (which DuckDB cannot see as a write)
	[[noreturn]] static void ThrowReadOnly(const string &database_name);
	[[noreturn]] static void ThrowReadOnly(const Identifier &database_name) {
		ThrowReadOnly(database_name.GetIdentifierName());
	}
	//! 10^exponent (exponent <= 18)
	static int64_t PowerOfTen(idx_t exponent);
	//! Rescales ticks between decimal precisions, flooring when precision is lost
	static int64_t ScaleTicks(int64_t ticks, idx_t from_precision, idx_t to_precision);
};

} // namespace duckdb
