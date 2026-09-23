#include "clickhouse_secrets.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

SecretType ClickhouseSecrets::CreateType() {
	SecretType secret_type;
	secret_type.name = TYPE_NAME;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	return secret_type;
}

unique_ptr<BaseSecret> ClickhouseSecrets::CreateFunction(ClientContext &context, CreateSecretInput &input) {
	vector<string> prefix_paths;
	auto result = make_uniq<KeyValueSecret>(prefix_paths, TYPE_NAME, "config", input.name);
	// validate every option now so that a broken secret fails at CREATE SECRET rather than at ATTACH
	ClickhouseConnectionConfig validation;
	for (auto &named_param : input.options) {
		auto key = StringUtil::Lower(named_param.first);
		auto value = named_param.second.ToString();
		validation.SetOption(key, value);
		result->secret_map[key] = Value(value);
	}
	result->redact_keys = {"password"};
	return std::move(result);
}

void ClickhouseSecrets::SetSecretParameters(CreateSecretFunction &function) {
	for (auto &name : ClickhouseConnectionConfig::OptionNames()) {
		function.named_parameters[name] = LogicalType::VARCHAR;
	}
}

unique_ptr<SecretEntry> ClickhouseSecrets::GetSecretEntry(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto name = secret_name.empty() ? string(DEFAULT_SECRET_NAME) : secret_name;
	auto entry = secret_manager.GetSecretByName(transaction, name);
	if (!entry) {
		entry = secret_manager.GetSecretByName(transaction, name, "local_file");
	}
	if (!entry) {
		if (!secret_name.empty()) {
			throw BinderException("Secret with name \"%s\" not found", secret_name);
		}
		return nullptr;
	}
	if (entry->secret->GetType() != TYPE_NAME) {
		throw BinderException("Secret \"%s\" is not a ClickHouse secret (it has type \"%s\")", name,
		                      entry->secret->GetType());
	}
	return entry;
}

void ClickhouseSecrets::ApplySecret(const SecretEntry &entry, ClickhouseConnectionConfig &config) {
	auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*entry.secret);
	for (auto &item : kv_secret.secret_map) {
		config.SetOption(item.first, item.second.ToString());
	}
}

} // namespace duckdb
