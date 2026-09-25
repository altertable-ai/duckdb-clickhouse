#include "storage/clickhouse_connection_pool.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

ClickhouseConnectionPool::ClickhouseConnectionPool(ClickhouseConnectionConfig config_p, ClickhouseTimeouts timeouts_p,
                                                   dbconnector::pool::ConnectionPoolConfig pool_config)
    : dbconnector::pool::ConnectionPool<ClickhouseConnection>(pool_config), config(std::move(config_p)),
      timeouts(timeouts_p) {
}

ClickhousePoolConnection ClickhouseConnectionPool::GetConnection() {
	return Acquire();
}

std::unique_ptr<ClickhouseConnection> ClickhouseConnectionPool::CreateNewConnection() {
	return ClickhouseConnection::Open(config, timeouts);
}

bool ClickhouseConnectionPool::CheckConnectionHealthy(ClickhouseConnection &connection) {
	return connection.IsHealthy();
}

void ClickhouseConnectionPool::ResetConnection(ClickhouseConnection &connection) {
	if (connection.IsQueryRunning()) {
		connection.Cancel();
	}
}

bool ClickhouseConnectionPool::TryRecoverConnection(ClickhouseConnection &connection) {
	if (connection.IsQueryRunning()) {
		connection.Cancel();
	}
	return !connection.IsBroken() && !connection.IsQueryRunning() && !connection.IsInserting();
}

dbconnector::pool::ConnectionPoolConfig ClickhouseConnectionPool::PoolConfigFromContext(ClientContext &context) {
	dbconnector::pool::ConnectionPoolConfig result;
	Value value;
	if (context.TryGetCurrentSetting("ch_pool_acquire_mode", value) && !value.IsNull()) {
		try {
			result.acquire_mode = dbconnector::pool::AcquireModeHelpers::FromString(value.ToString());
		} catch (std::exception &ex) {
			throw InvalidInputException("Invalid ch_pool_acquire_mode \"%s\": expected force, wait or try",
			                            value.ToString());
		}
	}
	if (context.TryGetCurrentSetting("ch_pool_max_connections", value) && !value.IsNull()) {
		result.max_connections = UBigIntValue::Get(value);
	}
	if (context.TryGetCurrentSetting("ch_pool_wait_timeout_millis", value) && !value.IsNull()) {
		result.wait_timeout_millis = UBigIntValue::Get(value);
	}
	if (context.TryGetCurrentSetting("ch_pool_idle_timeout_millis", value) && !value.IsNull()) {
		result.idle_timeout_millis = UBigIntValue::Get(value);
	}
	return result;
}

} // namespace duckdb
