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
	void BeginQuery(const string &sql, const vector<std::pair<string, string>> &query_settings = {});
	//! The next block (possibly with zero rows); nullopt once the query has finished
	std::optional<clickhouse::Block> NextBlock();
	//! Cancels the running query (if any): sends a cancel and reads (drains) the blocks the server sends
	//! afterward, leaving the connection idle and safe to reuse for a new query. No-op if no query is
	//! running. Marks the connection broken (see IsBroken()) if the cancel itself fails.
	void Cancel();
	bool IsQueryRunning() const;

	//! Runs a query to completion and returns all of its blocks. `query_settings` are sent like Execute()'s, after
	//! (so they override) the connection's own settings
	vector<clickhouse::Block> Query(const string &sql, const vector<std::pair<string, string>> &query_settings = {});

	//! Runs a statement that is not expected to return rows, e.g. DDL. Returns false -- after cancelling the rest
	//! of the result -- as soon as the statement returns a row, true once it has finished without returning any
	bool Execute(const string &sql);
	//! Same as Execute(sql), with extra query-level settings. They are sent as non-IMPORTANT settings (a server that
	//! does not know one ignores it), after the connection's own settings: a key in both takes this value
	bool Execute(const string &sql, const vector<std::pair<string, string>> &query_settings);

	//! Starts an INSERT (`INSERT INTO … VALUES` or `INSERT INTO … SELECT … FROM input(…)`) with the connection's
	//! settings plus `query_settings` (same semantics as Execute's), and returns the server's header block: one
	//! empty column per value sent, in order
	clickhouse::Block BeginInsert(const string &sql, const vector<std::pair<string, string>> &query_settings = {});
	//! Sends one block of rows for the INSERT started by BeginInsert()
	void SendInsertBlock(const clickhouse::Block &block);
	//! Finishes the INSERT started by BeginInsert(); errors ClickHouse reports while writing surface here
	void EndInsert();
	//! Abandons the INSERT started by BeginInsert(). The native protocol cannot cancel an INSERT, so the connection is
	//! marked broken: the pool closes it instead of reusing it, and ClickHouse aborts the query when the socket
	//! closes. No-op when no INSERT is running
	void AbortInsert();
	bool IsInserting() const;

	//! Usable for a new query: not broken, not mid-query or mid-insert, and answers a ping when it has been idle for
	//! 30s
	bool IsHealthy();
	//! True after a network or protocol error. Broken connections are never reused
	bool IsBroken() const;

	static void SetDebugPrintQueries(bool print);

	//! Query settings every query the extension issues itself runs with (the catalog's reads of system.databases,
	//! system.tables and system.columns, the counts and checks before a statement, the statement, an INSERT's
	//! conversions), whatever the ATTACH's settings= holds: transform_null_in = 0, no FINAL, no deleted rows, no
	//! filter, limit or partial result, strict parsing. A caller appends a setting after them to override one (see
	//! MakeQuery). The translated DML predicate does not depend on them (see ClickhouseExpression::InList)
	static vector<std::pair<string, string>> ExtensionQuerySettings();

private:
	clickhouse::Query MakeQuery(const string &sql, const vector<std::pair<string, string>> &query_settings = {}) const;
	//! Translates the in-flight exception into a DuckDB exception
	[[noreturn]] void RethrowAsDuckDBException(const string &sql);

	unique_ptr<clickhouse::Client> client;
	ClickhouseConnectionConfig config;
	bool broken = false;
	std::chrono::steady_clock::time_point last_used;

	static std::atomic<bool> debug_print_queries;
};

} // namespace duckdb
