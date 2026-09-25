#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/pair.hpp"

namespace duckdb {

//! Everything needed to open a ClickHouse connection. Built from (in order) a secret, the ATTACH path and the
//! ATTACH SETTINGS option; later sources override earlier ones.
struct ClickhouseConnectionConfig {
	string host = "localhost";
	//! 0 = not set: 9440 when secure, 9000 otherwise
	uint16_t port = 0;
	string user = "default";
	string password;
	string database = "default";
	//! -1 = not set (secure when port is 9440), 0 = false, 1 = true
	int8_t secure = -1;
	string ca_cert;
	bool skip_verify = false;
	//! lz4, zstd or none
	string compression = "lz4";
	//! ClickHouse settings sent with every query
	vector<pair<string, string>> settings;

	//! Applies a "key=value key2='quoted value'" string or a clickhouse[s]://user:password@host:port/db?k=v URI
	void ApplyConnectionString(const string &connection_string);
	//! Applies one option (case-insensitive key, aliases allowed). Throws InvalidInputException on unknown keys
	void SetOption(const string &key, const string &value);
	//! Appends settings written as "name1=value1,name2=value2"
	void AddSettings(const string &settings_text);

	bool IsSecure() const;
	uint16_t GetPort() const;
	//! clickhouse[s]://user@host:port/database (never includes the password)
	string ToDisplayString() const;

	//! All accepted option names, including aliases
	static const vector<string> &OptionNames();

private:
	void ApplyUri(const string &uri);
	void ApplyKeyValuePairs(const string &text);
};

} // namespace duckdb
