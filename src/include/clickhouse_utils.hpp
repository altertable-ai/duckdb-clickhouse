#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class ClickhouseUtils {
public:
	//! Quotes an identifier with backticks, escaping backslashes and backticks
	static string QuoteIdentifier(const string &identifier);
	//! Quotes a string literal with single quotes, escaping backslashes and single quotes
	static string QuoteLiteral(const string &literal);
	//! Returns true if the byte range is well-formed UTF-8
	static bool IsValidUtf8(const char *data, idx_t size);
	//! Removes trailing whitespace and semicolons, which the native protocol does not accept
	static string StripTrailingSemicolons(string sql);
	//! Throws the error for a write statement that attached ClickHouse databases do not support (yet)
	[[noreturn]] static void ThrowUnsupportedWrite(const string &statement);
	//! Throws the error for a write that reaches the extension on a database attached with READ_ONLY. DuckDB
	//! itself rejects write statements on such databases; this covers what DuckDB cannot see, e.g.
	//! clickhouse_execute()
	[[noreturn]] static void ThrowReadOnly(const string &database_name);
};

} // namespace duckdb
