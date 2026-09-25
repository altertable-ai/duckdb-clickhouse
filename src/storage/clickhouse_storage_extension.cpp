#include "storage/clickhouse_storage_extension.hpp"

#include "clickhouse_secrets.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_transaction.hpp"

namespace duckdb {

static unique_ptr<Catalog> ClickhouseAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                            AttachedDatabase &db, const string &name, AttachInfo &info,
                                            AttachOptions &attach_options) {
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("Attaching ClickHouse databases is disabled through configuration");
	}
	string secret_name;
	string settings;
	ClickhouseAttachOptions options;
	for (auto &entry : attach_options.options) {
		auto key = StringUtil::Lower(entry.first);
		if (key == "secret") {
			secret_name = entry.second.ToString();
		} else if (key == "settings") {
			settings = entry.second.ToString();
		} else if (key == "show_system") {
			options.show_system = BooleanValue::Get(entry.second.DefaultCastAs(LogicalType::BOOLEAN));
		} else if (key == "schema") {
			options.schema = entry.second.ToString();
			if (options.schema.empty()) {
				throw BinderException("SCHEMA must name a ClickHouse database");
			}
		} else {
			throw BinderException("Unrecognized option for ClickHouse attach: %s", entry.first);
		}
	}
	ClickhouseConnectionConfig config;
	auto secret_entry = ClickhouseSecrets::GetSecretEntry(context, secret_name);
	if (secret_entry) {
		ClickhouseSecrets::ApplySecret(*secret_entry, config);
	}
	config.ApplyConnectionString(info.path);
	if (!settings.empty()) {
		config.AddSettings(settings);
	}
	return make_uniq<ClickhouseCatalog>(db, std::move(config), options, context);
}

static unique_ptr<TransactionManager>
ClickhouseCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info, AttachedDatabase &db,
                                   Catalog &catalog) {
	return make_uniq<ClickhouseTransactionManager>(db);
}

ClickhouseStorageExtension::ClickhouseStorageExtension() {
	attach = ClickhouseAttach;
	create_transaction_manager = ClickhouseCreateTransactionManager;
}

} // namespace duckdb
