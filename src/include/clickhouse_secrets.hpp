#pragma once

#include "clickhouse_connection_config.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

class ClickhouseSecrets {
public:
	static constexpr const char *TYPE_NAME = "clickhouse";
	//! Name DuckDB gives an unnamed CREATE SECRET (TYPE clickhouse, ...)
	static constexpr const char *DEFAULT_SECRET_NAME = "__default_clickhouse";

	static SecretType CreateType();
	static unique_ptr<BaseSecret> CreateFunction(ClientContext &context, CreateSecretInput &input);
	static void SetSecretParameters(CreateSecretFunction &function);
	//! The named secret, or the default unnamed ClickHouse secret when secret_name is empty (may return nullptr).
	//! Throws when a named secret does not exist.
	static unique_ptr<SecretEntry> GetSecretEntry(ClientContext &context, const string &secret_name);
	static void ApplySecret(const SecretEntry &entry, ClickhouseConnectionConfig &config);
};

} // namespace duckdb
