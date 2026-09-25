#include "clickhouse_utils.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

static string QuoteWith(const string &text, char quote) {
	string result;
	result.reserve(text.size() + 2);
	result += quote;
	for (auto c : text) {
		if (c == '\\' || c == quote) {
			result += '\\';
		}
		result += c;
	}
	result += quote;
	return result;
}

string ClickhouseUtils::QuoteIdentifier(const string &identifier) {
	return QuoteWith(identifier, '`');
}

string ClickhouseUtils::QualifiedName(const string &database, const string &table) {
	return QuoteIdentifier(database) + "." + QuoteIdentifier(table);
}

string ClickhouseUtils::QuoteLiteral(const string &literal) {
	return QuoteWith(literal, '\'');
}

bool ClickhouseUtils::IsValidUtf8(const char *data, idx_t size) {
	auto bytes = reinterpret_cast<const uint8_t *>(data);
	idx_t i = 0;
	while (i < size) {
		auto c = bytes[i];
		if (c < 0x80) {
			i++;
			continue;
		}
		idx_t length;
		uint32_t code_point;
		if ((c & 0xE0) == 0xC0) {
			length = 2;
			code_point = c & 0x1F;
		} else if ((c & 0xF0) == 0xE0) {
			length = 3;
			code_point = c & 0x0F;
		} else if ((c & 0xF8) == 0xF0) {
			length = 4;
			code_point = c & 0x07;
		} else {
			return false;
		}
		if (i + length > size) {
			return false;
		}
		for (idx_t k = 1; k < length; k++) {
			auto continuation = bytes[i + k];
			if ((continuation & 0xC0) != 0x80) {
				return false;
			}
			code_point = (code_point << 6) | (continuation & 0x3F);
		}
		// reject overlong encodings, UTF-16 surrogates and code points beyond U+10FFFF
		if ((length == 2 && code_point < 0x80) || (length == 3 && code_point < 0x800) ||
		    (length == 4 && code_point < 0x10000) || code_point > 0x10FFFF ||
		    (code_point >= 0xD800 && code_point <= 0xDFFF)) {
			return false;
		}
		i += length;
	}
	return true;
}

string ClickhouseUtils::StripTrailingSemicolons(string sql) {
	StringUtil::RTrim(sql);
	while (!sql.empty() && sql.back() == ';') {
		sql.pop_back();
		StringUtil::RTrim(sql);
	}
	return sql;
}

void ClickhouseUtils::ThrowUnsupportedWrite(const string &statement) {
	throw NotImplementedException("%s is not supported on attached ClickHouse databases yet; run it in ClickHouse "
	                              "with clickhouse_execute() instead",
	                              statement);
}

void ClickhouseUtils::ThrowReadOnly(const string &database_name) {
	throw PermissionException("Cannot write to ClickHouse database \"%s\": it is attached in read-only mode",
	                          database_name);
}

int64_t ClickhouseUtils::PowerOfTen(idx_t exponent) {
	int64_t result = 1;
	for (idx_t i = 0; i < exponent; i++) {
		result *= 10;
	}
	return result;
}

int64_t ClickhouseUtils::ScaleTicks(int64_t ticks, idx_t from_precision, idx_t to_precision) {
	if (from_precision == to_precision) {
		return ticks;
	}
	if (from_precision < to_precision) {
		return ticks * PowerOfTen(to_precision - from_precision);
	}
	auto divisor = PowerOfTen(from_precision - to_precision);
	auto result = ticks / divisor;
	if (ticks % divisor != 0 && ticks < 0) {
		result -= 1;
	}
	return result;
}

} // namespace duckdb
