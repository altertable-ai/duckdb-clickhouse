#define DUCKDB_EXTENSION_MAIN

#include "clickhouse_scanner_extension.hpp"

#include "clickhouse_connection.hpp"
#include "clickhouse_execute.hpp"
#include "clickhouse_scanner.hpp"
#include "clickhouse_secrets.hpp"
#include "clickhouse_type_mapping_function.hpp"
#include "dbconnector/pool.hpp"
#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "storage/clickhouse_clear_cache.hpp"
#include "storage/clickhouse_ddl.hpp"
#include "storage/clickhouse_optimizer.hpp"
#include "storage/clickhouse_storage_extension.hpp"

#include <clickhouse/client.h>

namespace duckdb {

static void ClickhouseClientVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto version = clickhouse::Client::GetVersion();
	auto text = StringUtil::Format("%d.%d.%d", version.major, version.minor, version.patch);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, text);
}

static void SetClickhouseDebugPrintQueries(ClientContext &context, SetScope scope, Value &parameter) {
	ClickhouseConnection::SetDebugPrintQueries(BooleanValue::Get(parameter));
}

static void SetClickhouseInsertBlockSize(ClientContext &context, SetScope scope, Value &parameter) {
	if (UBigIntValue::Get(parameter) == 0) {
		throw InvalidInputException("ch_insert_block_size must be greater than 0");
	}
}

static void SetClickhouseDefaultTableEngine(ClientContext &context, SetScope scope, Value &parameter) {
	ClickhouseDdl::ValidateEngine(StringValue::Get(parameter));
}

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunction version_function("clickhouse_client_version", {}, LogicalType::VARCHAR,
	                                ClickhouseClientVersionFunction);
	loader.RegisterFunction(version_function);
	loader.RegisterFunction(ClickhouseTypeMappingFunction());
	loader.RegisterFunction(ClickhouseClearCacheFunction());
	loader.RegisterFunction(ClickhouseScanFunction());
	loader.RegisterFunction(ClickhouseQueryFunction());
	loader.RegisterFunction(ClickhouseExecuteFunction());

	loader.RegisterSecretType(ClickhouseSecrets::CreateType());
	CreateSecretFunction secret_function = {ClickhouseSecrets::TYPE_NAME, "config", ClickhouseSecrets::CreateFunction};
	ClickhouseSecrets::SetSecretParameters(secret_function);
	loader.RegisterFunction(secret_function);

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	auto storage_extension = make_shared_ptr<ClickhouseStorageExtension>();
	StorageExtension::Register(config, "clickhouse_scanner", storage_extension);
	StorageExtension::Register(config, "clickhouse", storage_extension);

	dbconnector::pool::ConnectionPoolConfig default_pool_config;
	config.AddExtensionOption("ch_debug_show_queries", "DEBUG SETTING: print all queries sent to ClickHouse to stdout",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), SetClickhouseDebugPrintQueries);
	config.AddExtensionOption("ch_filter_pushdown", "Push filters down into the queries sent to ClickHouse",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption("ch_connect_timeout_ms", "Timeout in milliseconds for connecting to ClickHouse",
	                          LogicalType::UBIGINT, Value::UBIGINT(10000));
	config.AddExtensionOption("ch_receive_timeout_ms", "Timeout in milliseconds for receiving data from ClickHouse",
	                          LogicalType::UBIGINT, Value::UBIGINT(300000));
	config.AddExtensionOption("ch_pool_max_connections",
	                          "Maximum number of pooled connections per attached ClickHouse database (new ATTACHes)",
	                          LogicalType::UBIGINT, Value::UBIGINT(default_pool_config.max_connections));
	config.AddExtensionOption("ch_pool_acquire_mode",
	                          "What to do when the pool is exhausted: force, wait or try (new ATTACHes)",
	                          LogicalType::VARCHAR, Value("force"));
	config.AddExtensionOption("ch_pool_wait_timeout_millis",
	                          "How long 'wait' acquire mode waits for a free connection (new ATTACHes)",
	                          LogicalType::UBIGINT, Value::UBIGINT(default_pool_config.wait_timeout_millis));
	config.AddExtensionOption("ch_pool_idle_timeout_millis",
	                          "Idle pooled connections are closed after this many milliseconds (new ATTACHes)",
	                          LogicalType::UBIGINT, Value::UBIGINT(default_pool_config.idle_timeout_millis));
	config.AddExtensionOption("ch_order_pushdown", "Push LIMIT and ORDER BY ... LIMIT down into ClickHouse queries",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption("ch_insert_block_size",
	                          "Minimum rows per block sent to ClickHouse during INSERT (rounded up to whole chunks)",
	                          LogicalType::UBIGINT, Value::UBIGINT(65536), SetClickhouseInsertBlockSize);
	config.AddExtensionOption("ch_default_table_engine",
	                          "Table engine for CREATE TABLE in attached ClickHouse databases, e.g. MergeTree or "
	                          "ReplicatedMergeTree('/clickhouse/tables/{shard}/{database}/{table}', '{replica}')",
	                          LogicalType::VARCHAR, Value("MergeTree"), SetClickhouseDefaultTableEngine);
	OptimizerExtension clickhouse_optimizer;
	clickhouse_optimizer.optimize_function = ClickhouseOptimizer::Optimize;
	OptimizerExtension::Register(config, std::move(clickhouse_optimizer));
}

void ClickhouseScannerExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string ClickhouseScannerExtension::Name() {
	return "clickhouse_scanner";
}

std::string ClickhouseScannerExtension::Version() const {
#ifdef EXT_VERSION_CLICKHOUSE_SCANNER
	return EXT_VERSION_CLICKHOUSE_SCANNER;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(clickhouse_scanner, loader) {
	duckdb::LoadInternal(loader);
}
}
