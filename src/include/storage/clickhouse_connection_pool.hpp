#pragma once

#include "clickhouse_connection.hpp"
#include "dbconnector/pool.hpp"

namespace duckdb {
class ClientContext;

using ClickhousePoolConnection = dbconnector::pool::PooledConnection<ClickhouseConnection>;

class ClickhouseConnectionPool : public dbconnector::pool::ConnectionPool<ClickhouseConnection> {
public:
	ClickhouseConnectionPool(ClickhouseConnectionConfig config, ClickhouseTimeouts timeouts,
	                         dbconnector::pool::ConnectionPoolConfig pool_config);

	ClickhousePoolConnection GetConnection();

	//! Reads the ch_pool_* settings
	static dbconnector::pool::ConnectionPoolConfig PoolConfigFromContext(ClientContext &context);

protected:
	std::unique_ptr<ClickhouseConnection> CreateNewConnection() override;
	bool CheckConnectionHealthy(ClickhouseConnection &connection) override;
	void ResetConnection(ClickhouseConnection &connection) override;
	//! CheckConnectionHealthy() is false while a query is running (IsHealthy() reports unusable-right-now,
	//! not unrecoverable), which would otherwise make the pool discard every connection returned mid-query
	//! (e.g. a LIMIT-terminated scan). Cancels the in-flight query so the connection can be reset and reused.
	//! An INSERT cannot be cancelled, so a connection returned mid-insert is never recovered.
	bool TryRecoverConnection(ClickhouseConnection &connection) override;

private:
	ClickhouseConnectionConfig config;
	ClickhouseTimeouts timeouts;
};

} // namespace duckdb
