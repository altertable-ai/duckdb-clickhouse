#include "clickhouse_connection.hpp"

#include "clickhouse_error_codes.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

#include <fstream>

namespace duckdb {

std::atomic<bool> ClickhouseConnection::debug_print_queries {false};

//! LowCardinality columns are sent as plain columns, so the conversion code never sees dictionary encoding
static constexpr const char *LOW_CARDINALITY_SETTING = "low_cardinality_allow_in_native_format";
static constexpr auto PING_AFTER_IDLE = std::chrono::seconds(30);

static bool IsUpperSnakeCase(const string &value) {
	if (value.empty()) {
		return false;
	}
	bool has_alpha = false;
	for (auto c : value) {
		if (c >= 'A' && c <= 'Z') {
			has_alpha = true;
		} else if (!((c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
	}
	return has_alpha;
}

//! Some ClickHouse exception paths embed the symbolic error code name at the end of display_text, e.g.
//! "DB::Exception: Table default.t doesn't exist. (UNKNOWN_TABLE)" -- verified this is NOT the case for
//! a native-protocol connection to a plain (non-distributed) ClickHouse 25.8 server, where display_text
//! is always just "DB::Exception: <message>" with no trailing "(NAME)"; that annotation turned out to be
//! something clickhouse-client's terminal formatter adds locally from the numeric code, not something
//! ClickHouse puts on the wire. So: try to extract a trailing "(NAME)" token first (in case some other
//! server version or a wrapped/distributed-query exception does include one), then fall back to our own
//! code -> name table (see clickhouse_error_codes.hpp), and only fall back to error.name (just the
//! exception class, e.g. "DB::Exception") if the code itself is unrecognized.
static string ErrorCodeName(const clickhouse::Exception &error) {
	const string &text = error.display_text;
	idx_t end = text.size();
	for (int attempt = 0; attempt < 3; attempt++) {
		// only a "(NAME)" anchored to the end (ignoring trailing whitespace) counts -- a parenthesized token
		// elsewhere in a long message (e.g. a syntax error echoing the offending token, "... (THIS): ...")
		// is not the symbolic error code
		while (end > 0 && StringUtil::CharacterIsSpace(text[end - 1])) {
			end--;
		}
		if (end == 0 || text[end - 1] != ')') {
			break;
		}
		auto close = end - 1;
		auto open = text.rfind('(', close);
		if (open == string::npos) {
			break;
		}
		auto candidate = text.substr(open + 1, close - open - 1);
		if (IsUpperSnakeCase(candidate)) {
			return candidate;
		}
		end = open;
	}
	auto looked_up = ClickhouseErrorCodeName(error.code);
	if (looked_up[0] != '\0') {
		return looked_up;
	}
	return error.name;
}

//! Strips a leading "DB::Exception: " so it is not duplicated after "ClickHouse error N (NAME): "
static string StripExceptionPrefix(const string &display_text) {
	static const string PREFIX = "DB::Exception: ";
	if (StringUtil::StartsWith(display_text, PREFIX)) {
		return display_text.substr(PREFIX.size());
	}
	return display_text;
}

ClickhouseTimeouts ClickhouseTimeouts::FromContext(ClientContext &context) {
	ClickhouseTimeouts result;
	Value value;
	if (context.TryGetCurrentSetting("ch_connect_timeout_ms", value) && !value.IsNull()) {
		result.connect_timeout_ms = UBigIntValue::Get(value);
	}
	if (context.TryGetCurrentSetting("ch_receive_timeout_ms", value) && !value.IsNull()) {
		result.receive_timeout_ms = UBigIntValue::Get(value);
	}
	return result;
}

//! vcpkg's OpenSSL does not know where the operating system keeps its CA bundle
static string FindSystemCABundle() {
	static const char *CANDIDATES[] = {"/etc/ssl/certs/ca-certificates.crt",
	                                   "/etc/pki/tls/certs/ca-bundle.crt",
	                                   "/etc/ssl/ca-bundle.pem",
	                                   "/etc/pki/tls/cacert.pem",
	                                   "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
	                                   "/etc/ssl/cert.pem"};
	for (auto candidate : CANDIDATES) {
		std::ifstream file(candidate);
		if (file.good()) {
			return candidate;
		}
	}
	return string();
}

static clickhouse::ClientOptions MakeClientOptions(const ClickhouseConnectionConfig &config,
                                                   const ClickhouseTimeouts &timeouts) {
	clickhouse::ClientOptions options;
	options.SetHost(config.host);
	options.SetPort(config.GetPort());
	options.SetUser(config.user);
	options.SetPassword(config.password);
	options.SetDefaultDatabase(config.database);
	options.SetRethrowException(true);
	options.SetSendRetries(1);
	options.TcpKeepAlive(true);
	options.SetConnectionConnectTimeout(std::chrono::milliseconds(timeouts.connect_timeout_ms));
	options.SetConnectionRecvTimeout(std::chrono::milliseconds(timeouts.receive_timeout_ms));
	if (config.compression == "lz4") {
		options.SetCompressionMethod(clickhouse::CompressionMethod::LZ4);
	} else if (config.compression == "zstd") {
		options.SetCompressionMethod(clickhouse::CompressionMethod::ZSTD);
	} else {
		options.SetCompressionMethod(clickhouse::CompressionMethod::None);
	}
	if (config.IsSecure()) {
		clickhouse::ClientOptions::SSLOptions ssl_options;
		if (!config.ca_cert.empty()) {
			ssl_options.SetPathToCAFiles({config.ca_cert});
			ssl_options.SetUseDefaultCALocations(false);
		} else {
			auto bundle = FindSystemCABundle();
			if (!bundle.empty()) {
				ssl_options.SetPathToCAFiles({bundle});
			}
		}
		ssl_options.SetSkipVerification(config.skip_verify);
		options.SetSSLOptions(std::move(ssl_options));
	}
	return options;
}

ClickhouseConnection::ClickhouseConnection(unique_ptr<clickhouse::Client> client_p, ClickhouseConnectionConfig config_p)
    : client(std::move(client_p)), config(std::move(config_p)), last_used(std::chrono::steady_clock::now()) {
}

ClickhouseConnection::~ClickhouseConnection() = default;

unique_ptr<ClickhouseConnection> ClickhouseConnection::Open(const ClickhouseConnectionConfig &config,
                                                            const ClickhouseTimeouts &timeouts) {
	try {
		auto client = make_uniq<clickhouse::Client>(MakeClientOptions(config, timeouts));
		return make_uniq<ClickhouseConnection>(std::move(client), config);
	} catch (const clickhouse::ServerException &ex) {
		auto &error = ex.GetException();
		throw IOException("Failed to connect to ClickHouse at %s:%d: ClickHouse error %d (%s): %s", config.host,
		                  static_cast<int32_t>(config.GetPort()), error.code, ErrorCodeName(error),
		                  StripExceptionPrefix(error.display_text));
	} catch (const std::exception &ex) {
		throw IOException("Failed to connect to ClickHouse at %s:%d: %s", config.host,
		                  static_cast<int32_t>(config.GetPort()), ex.what());
	}
}

void ClickhouseConnection::SetDebugPrintQueries(bool print) {
	debug_print_queries = print;
}

vector<std::pair<string, string>> ClickhouseConnection::ExtensionQuerySettings() {
	// transform_null_in = 1 would make a bare x NOT IN (...) true for a NULL x (DuckDB: NULL, the row is kept).
	// ClickhouseExpression::InList guards every IN / NOT IN against it, in the count and the statement alike. The pins
	// reach only ordinary queries: DELETE and ALTER TABLE … UPDATE run with the server's default profile as loaded at
	// startup, whatever the query's settings say.
	// The rest make a count read every stored row a mutation rewrites, and read all of its result: FINAL would hide
	// replaced rows, apply_deleted_mask = 0 would count deleted ones, the overflow modes set to 'break' and the result
	// limits would return a partial count or listing, and the filters, limit and offset would drop rows or the result
	// row itself. A cached result may be stale. CAST must not turn an IP that does not parse into 0.0.0.0, nor keep
	// the first of a JSON object's duplicated paths, and if(isNull(x), NULL, CAST(…)) must not convert the NULLs
	return {{"transform_null_in", "0"},
	        {"final", "0"},
	        {"apply_deleted_mask", "1"},
	        {"read_overflow_mode", "throw"},
	        {"read_overflow_mode_leaf", "throw"},
	        {"timeout_overflow_mode", "throw"},
	        {"timeout_overflow_mode_leaf", "throw"},
	        {"set_overflow_mode", "throw"},
	        {"group_by_overflow_mode", "throw"},
	        {"result_overflow_mode", "throw"},
	        {"max_result_rows", "0"},
	        {"max_result_bytes", "0"},
	        {"additional_table_filters", "{}"},
	        {"additional_result_filter", ""},
	        {"limit", "0"},
	        {"offset", "0"},
	        {"use_query_cache", "0"},
	        {"cast_ipv4_ipv6_default_on_conversion_error", "0"},
	        {"type_json_skip_duplicated_paths", "0"},
	        {"short_circuit_function_evaluation", "enable"}};
}

clickhouse::Query ClickhouseConnection::MakeQuery(const string &sql,
                                                  const vector<std::pair<string, string>> &query_settings) const {
	clickhouse::Query query(sql);
	for (auto &setting : config.settings) {
		// IMPORTANT makes the server reject unknown settings instead of silently ignoring them
		query.SetSetting(setting.first,
		                 clickhouse::QuerySettingsField {setting.second, clickhouse::QuerySettingsField::IMPORTANT});
	}
	// after the connection's settings: clickhouse::Query keeps one value per key (the last one set), so a query
	// setting overrides the same key from the ATTACH's settings=
	for (auto &setting : query_settings) {
		query.SetSetting(setting.first, clickhouse::QuerySettingsField {setting.second, 0});
	}
	// Applied last, after the user's settings, so a "settings=" value can never re-enable LowCardinality's
	// dictionary encoding: AddSettings() also rejects this key outright, this is a second line of defense.
	query.SetSetting(LOW_CARDINALITY_SETTING, clickhouse::QuerySettingsField {"0", 0});
	return query;
}

void ClickhouseConnection::RethrowAsDuckDBException(const string &sql) {
	auto query_suffix = debug_print_queries && !sql.empty() ? "\nQuery: " + sql : string();
	try {
		throw;
	} catch (const clickhouse::ServerException &ex) {
		// the server reported an error; the connection itself is still in a clean state
		auto &error = ex.GetException();
		throw IOException("ClickHouse error %d (%s): %s%s", error.code, ErrorCodeName(error),
		                  StripExceptionPrefix(error.display_text), query_suffix);
	} catch (const Exception &) {
		broken = true;
		throw;
	} catch (const std::exception &ex) {
		broken = true;
		throw IOException("ClickHouse connection error (%s:%d): %s%s", config.host,
		                  static_cast<int32_t>(config.GetPort()), ex.what(), query_suffix);
	}
}

void ClickhouseConnection::BeginQuery(const string &sql, const vector<std::pair<string, string>> &query_settings) {
	if (debug_print_queries) {
		Printer::Print(sql + "\n");
	}
	last_used = std::chrono::steady_clock::now();
	try {
		client->BeginSelect(MakeQuery(sql, query_settings));
	} catch (...) {
		RethrowAsDuckDBException(sql);
	}
}

std::optional<clickhouse::Block> ClickhouseConnection::NextBlock() {
	try {
		auto block = client->NextBlock();
		last_used = std::chrono::steady_clock::now();
		return block;
	} catch (...) {
		RethrowAsDuckDBException(string());
	}
}

void ClickhouseConnection::Cancel() {
	if (!client->IsSelecting()) {
		return;
	}
	try {
		client->Cancel();
	} catch (...) {
		broken = true;
	}
}

bool ClickhouseConnection::IsQueryRunning() const {
	return client->IsSelecting();
}

vector<clickhouse::Block> ClickhouseConnection::Query(const string &sql,
                                                      const vector<std::pair<string, string>> &query_settings) {
	vector<clickhouse::Block> result;
	BeginQuery(sql, query_settings);
	while (true) {
		auto block = NextBlock();
		if (!block) {
			break;
		}
		result.push_back(std::move(*block));
	}
	return result;
}

bool ClickhouseConnection::Execute(const string &sql) {
	return Execute(sql, {});
}

bool ClickhouseConnection::Execute(const string &sql, const vector<std::pair<string, string>> &query_settings) {
	BeginQuery(sql, query_settings);
	while (auto block = NextBlock()) {
		if (block->GetRowCount() > 0) {
			Cancel();
			return false;
		}
	}
	return true;
}

clickhouse::Block ClickhouseConnection::BeginInsert(const string &sql,
                                                    const vector<std::pair<string, string>> &query_settings) {
	if (debug_print_queries) {
		Printer::Print(sql + "\n");
	}
	last_used = std::chrono::steady_clock::now();
	try {
		return client->BeginInsert(MakeQuery(sql, query_settings));
	} catch (...) {
		// clickhouse-cpp stays in its "inserting" state after a failed BeginInsert, even for a server error
		// (e.g. an unknown table), so this connection cannot run anything else
		broken = true;
		RethrowAsDuckDBException(sql);
	}
}

void ClickhouseConnection::SendInsertBlock(const clickhouse::Block &block) {
	last_used = std::chrono::steady_clock::now();
	try {
		client->SendInsertBlock(block);
	} catch (...) {
		broken = true;
		RethrowAsDuckDBException(string());
	}
}

void ClickhouseConnection::EndInsert() {
	last_used = std::chrono::steady_clock::now();
	try {
		client->EndInsert();
	} catch (...) {
		broken = true;
		RethrowAsDuckDBException(string());
	}
}

void ClickhouseConnection::AbortInsert() {
	if (client->IsInserting()) {
		broken = true;
	}
}

bool ClickhouseConnection::IsInserting() const {
	return client->IsInserting();
}

bool ClickhouseConnection::IsHealthy() {
	if (broken || client->IsSelecting() || client->IsInserting()) {
		return false;
	}
	auto now = std::chrono::steady_clock::now();
	if (now - last_used < PING_AFTER_IDLE) {
		return true;
	}
	try {
		client->Ping();
		last_used = now;
		return true;
	} catch (...) {
		broken = true;
		return false;
	}
}

bool ClickhouseConnection::IsBroken() const {
	return broken;
}

} // namespace duckdb
