#pragma once

#include "clickhouse_connection_config.hpp"
#include "duckdb/common/common.hpp"

#include <atomic>
#include <chrono>
#include <optional>

#include <clickhouse/client.h>

namespace duckdb {
class ClientContext;

struct ClickhouseTimeouts {
	uint64_t connect_timeout_ms = 10000;
	uint64_t receive_timeout_ms = 300000;

	//! Reads ch_connect_timeout_ms / ch_receive_timeout_ms
	static ClickhouseTimeouts FromContext(ClientContext &context);
};

//! One native-protocol connection to ClickHouse. Not thread-safe: callers serialize access.
class ClickhouseConnection {
public:
	ClickhouseConnection(unique_ptr<clickhouse::Client> client, ClickhouseConnectionConfig config);
	~ClickhouseConnection();

	//! Connects. Throws IOException("Failed to connect to ClickHouse at host:port: ...") on failure
	static unique_ptr<ClickhouseConnection> Open(const ClickhouseConnectionConfig &config,
	                                             const ClickhouseTimeouts &timeouts);

	//! Starts a streaming query; read its blocks with NextBlock()
	void BeginQuery(const string &sql);
	//! The next block (possibly with zero rows); nullopt once the query has finished
	std::optional<clickhouse::Block> NextBlock();
	//! Cancels the running query (if any) and drains the connection
	void Cancel();
	bool IsQueryRunning() const;

	//! Runs a query to completion and returns all of its blocks
	vector<clickhouse::Block> Query(const string &sql);

	//! Usable for a new query: not broken, not mid-query, and answers a ping when it has been idle for 30s
	bool IsHealthy();
	//! True after a network or protocol error. Broken connections are never reused
	bool IsBroken() const;

	static void SetDebugPrintQueries(bool print);

private:
	clickhouse::Query MakeQuery(const string &sql) const;
	//! Translates the in-flight exception into a DuckDB exception
	[[noreturn]] void RethrowAsDuckDBException(const string &sql);

	unique_ptr<clickhouse::Client> client;
	ClickhouseConnectionConfig config;
	bool broken = false;
	std::chrono::steady_clock::time_point last_used;

	static std::atomic<bool> debug_print_queries;
};

} // namespace duckdb
