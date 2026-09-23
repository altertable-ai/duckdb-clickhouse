#include "clickhouse_connection_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

const vector<string> &ClickhouseConnectionConfig::OptionNames() {
	static const vector<string> OPTION_NAMES = {"host",     "port",        "user",        "password", "database",
	                                            "secure",   "ca_cert",     "skip_verify", "compression",
	                                            "settings", "username",    "dbname",      "hostname"};
	return OPTION_NAMES;
}

static bool ParseBoolean(const string &key, const string &value) {
	auto lower = StringUtil::Lower(value);
	if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
		return true;
	}
	if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
		return false;
	}
	throw InvalidInputException("Invalid value \"%s\" for ClickHouse option \"%s\": expected a boolean", value, key);
}

static uint16_t ParsePort(const string &value) {
	if (value.empty() || value.size() > 5) {
		throw InvalidInputException("Invalid ClickHouse port \"%s\"", value);
	}
	uint32_t port = 0;
	for (auto c : value) {
		if (!StringUtil::CharacterIsDigit(c)) {
			throw InvalidInputException("Invalid ClickHouse port \"%s\"", value);
		}
		port = port * 10 + static_cast<uint32_t>(c - '0');
	}
	if (port == 0 || port > 65535) {
		throw InvalidInputException("Invalid ClickHouse port \"%s\"", value);
	}
	return static_cast<uint16_t>(port);
}

static bool IsValidSettingName(const string &name) {
	if (name.empty() || !(StringUtil::CharacterIsAlpha(name[0]) || name[0] == '_')) {
		return false;
	}
	for (auto c : name) {
		if (!(StringUtil::CharacterIsAlphaNumeric(c) || c == '_')) {
			return false;
		}
	}
	return true;
}

void ClickhouseConnectionConfig::SetOption(const string &key_p, const string &value) {
	auto key = StringUtil::Lower(key_p);
	if (key == "username") {
		key = "user";
	} else if (key == "dbname") {
		key = "database";
	} else if (key == "hostname") {
		key = "host";
	}
	if (key == "host") {
		if (value.empty()) {
			throw InvalidInputException("ClickHouse option \"host\" cannot be empty");
		}
		host = value;
	} else if (key == "port") {
		port = ParsePort(value);
	} else if (key == "user") {
		user = value;
	} else if (key == "password") {
		password = value;
	} else if (key == "database") {
		database = value;
	} else if (key == "secure") {
		secure = ParseBoolean(key, value) ? 1 : 0;
	} else if (key == "ca_cert") {
		ca_cert = value;
	} else if (key == "skip_verify") {
		skip_verify = ParseBoolean(key, value);
	} else if (key == "compression") {
		auto lower = StringUtil::Lower(value);
		if (lower != "lz4" && lower != "zstd" && lower != "none") {
			throw InvalidInputException(
			    "Invalid value \"%s\" for ClickHouse option \"compression\": expected lz4, zstd or none", value);
		}
		compression = lower;
	} else if (key == "settings") {
		AddSettings(value);
	} else {
		throw InvalidInputException("Unknown ClickHouse connection option \"%s\"", key_p);
	}
}

void ClickhouseConnectionConfig::AddSettings(const string &settings_text) {
	for (auto &entry : StringUtil::Split(settings_text, ',')) {
		auto item = entry;
		StringUtil::Trim(item);
		if (item.empty()) {
			continue;
		}
		auto equals = item.find('=');
		string name = equals == string::npos ? item : item.substr(0, equals);
		StringUtil::Trim(name);
		if (equals == string::npos || !IsValidSettingName(name)) {
			throw InvalidInputException("Invalid ClickHouse setting \"%s\": expected name=value", item);
		}
		if (StringUtil::Lower(name) == "low_cardinality_allow_in_native_format") {
			// the block-conversion code assumes LowCardinality columns always arrive as plain columns;
			// letting a user setting re-enable dictionary encoding would silently corrupt scans
			throw InvalidInputException("ClickHouse setting \"low_cardinality_allow_in_native_format\" cannot be "
			                            "set: clickhouse_scanner requires it to stay disabled");
		}
		auto value = item.substr(equals + 1);
		StringUtil::Trim(value);
		settings.emplace_back(name, value);
	}
}

void ClickhouseConnectionConfig::ApplyConnectionString(const string &connection_string) {
	auto text = connection_string;
	StringUtil::Trim(text);
	if (text.empty()) {
		return;
	}
	if (StringUtil::StartsWith(text, "clickhouse://") || StringUtil::StartsWith(text, "clickhouses://")) {
		ApplyUri(text);
	} else {
		ApplyKeyValuePairs(text);
	}
}

void ClickhouseConnectionConfig::ApplyKeyValuePairs(const string &text) {
	idx_t pos = 0;
	while (true) {
		while (pos < text.size() && StringUtil::CharacterIsSpace(text[pos])) {
			pos++;
		}
		if (pos >= text.size()) {
			return;
		}
		string key;
		while (pos < text.size() && text[pos] != '=' && !StringUtil::CharacterIsSpace(text[pos])) {
			key += text[pos++];
		}
		while (pos < text.size() && StringUtil::CharacterIsSpace(text[pos])) {
			pos++;
		}
		if (pos >= text.size() || text[pos] != '=') {
			// `key` here is just the token the scanner stopped on: if the previous value was an unquoted
			// password containing a space, this token is actually a leftover fragment of that password, so
			// it must never be echoed back
			throw InvalidInputException("Invalid ClickHouse connection string: expected \"key=value\" pairs "
			                            "separated by spaces (a value containing a space must be quoted, e.g. "
			                            "password='my pass')");
		}
		pos++;
		while (pos < text.size() && StringUtil::CharacterIsSpace(text[pos])) {
			pos++;
		}
		string value;
		if (pos < text.size() && text[pos] == '\'') {
			pos++;
			bool closed = false;
			while (pos < text.size()) {
				if (text[pos] == '\\' && pos + 1 < text.size()) {
					value += text[pos + 1];
					pos += 2;
				} else if (text[pos] == '\'') {
					closed = true;
					pos++;
					break;
				} else {
					value += text[pos++];
				}
			}
			if (!closed) {
				throw InvalidInputException(
				    "Invalid ClickHouse connection string: unterminated quoted value for \"%s\"", key);
			}
		} else {
			while (pos < text.size() && !StringUtil::CharacterIsSpace(text[pos])) {
				value += text[pos++];
			}
		}
		SetOption(key, value);
	}
}

void ClickhouseConnectionConfig::ApplyUri(const string &uri) {
	bool secure_scheme = StringUtil::StartsWith(uri, "clickhouses://");
	auto rest = uri.substr(secure_scheme ? 14 : 13);
	if (secure_scheme) {
		secure = 1;
	}

	// Rule (documented since it is more lenient than RFC 3986): the authority is bounded by the first
	// '/', '?' or '#' that appears AT OR AFTER the last '@' in the string, rather than simply the first
	// '/', '?' or '#' after the scheme. Anchoring on the last '@' means an unencoded '/' or '?' inside
	// the password does not truncate the authority before the real "@host" boundary is reached -- the
	// leftover password fragment would otherwise be misread as the start of the path/query and could
	// leak into a port/host parse error. Percent-encoding '@', '/', '?' and '#' in the password is still
	// recommended and avoids relying on this fallback; a stray '@' inside an unencoded query value (after
	// the real authority) is a known, accepted edge case of this simpler rule.
	auto last_at = rest.find_last_of('@');
	auto authority_end = rest.find_first_of("/?#", last_at == string::npos ? 0 : last_at);
	string authority = authority_end == string::npos ? rest : rest.substr(0, authority_end);
	string remainder = authority_end == string::npos ? string() : rest.substr(authority_end);

	if (last_at != string::npos) {
		auto user_info = authority.substr(0, last_at);
		authority = authority.substr(last_at + 1);
		auto colon = user_info.find(':');
		if (colon == string::npos) {
			user = StringUtil::URLDecode(user_info);
		} else {
			user = StringUtil::URLDecode(user_info.substr(0, colon));
			password = StringUtil::URLDecode(user_info.substr(colon + 1));
		}
	}

	string path;
	string query;
	if (!remainder.empty()) {
		if (remainder[0] == '/') {
			auto question = remainder.find('?');
			path = question == string::npos ? remainder.substr(1) : remainder.substr(1, question - 1);
			if (question != string::npos) {
				query = remainder.substr(question + 1);
			}
		} else if (remainder[0] == '?') {
			query = remainder.substr(1);
		}
		// a leading '#' carries neither a path nor a query and is ignored
	}

	// From here `authority` holds only host[:port] or [ipv6][:port]; any credentials were already
	// extracted above, so none of the errors below can echo a password.
	if (!authority.empty() && authority[0] == '[') {
		// IPv6 literal: [::1]:9000
		auto close = authority.find(']');
		if (close == string::npos) {
			throw InvalidInputException("Invalid ClickHouse URI: unterminated IPv6 address literal (percent-encode "
			                            "'@', '/', '?' and '#' if they appear in the password)");
		}
		host = authority.substr(1, close - 1);
		auto after = authority.substr(close + 1);
		if (!after.empty()) {
			if (after[0] != ':') {
				throw InvalidInputException("Invalid ClickHouse URI: unexpected characters after the IPv6 address");
			}
			port = ParsePort(after.substr(1));
		}
	} else {
		auto colon = authority.rfind(':');
		if (colon != string::npos) {
			port = ParsePort(authority.substr(colon + 1));
			authority = authority.substr(0, colon);
		}
		if (!authority.empty()) {
			host = authority;
		}
	}
	if (!path.empty()) {
		database = StringUtil::URLDecode(path);
	}
	for (auto &parameter : StringUtil::Split(query, '&')) {
		if (parameter.empty()) {
			continue;
		}
		auto equals = parameter.find('=');
		if (equals == string::npos) {
			// the segment may be an unencoded password fragment (e.g. a stray "?password:x" typo); never
			// echo it back since it cannot be reliably attributed to a non-sensitive key
			throw InvalidInputException("Invalid ClickHouse URI query parameter: expected \"key=value\"");
		}
		auto key = StringUtil::URLDecode(parameter.substr(0, equals));
		auto value = StringUtil::URLDecode(parameter.substr(equals + 1));
		SetOption(key, value);
	}
}

bool ClickhouseConnectionConfig::IsSecure() const {
	if (secure == -1) {
		return port == 9440;
	}
	return secure == 1;
}

uint16_t ClickhouseConnectionConfig::GetPort() const {
	if (port != 0) {
		return port;
	}
	return IsSecure() ? 9440 : 9000;
}

string ClickhouseConnectionConfig::ToDisplayString() const {
	return StringUtil::Format("%s://%s@%s:%d/%s", IsSecure() ? "clickhouses" : "clickhouse", user, host,
	                          static_cast<int32_t>(GetPort()), database);
}

} // namespace duckdb
