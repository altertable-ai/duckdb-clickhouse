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
	//! Throws the error used for every write attempt against an attached ClickHouse database
	[[noreturn]] static void ThrowReadOnly();
};

} // namespace duckdb
