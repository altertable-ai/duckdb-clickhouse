#include "clickhouse_connection.hpp"

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
		                  static_cast<int32_t>(config.GetPort()), error.code, error.name, error.display_text);
	} catch (const std::exception &ex) {
		throw IOException("Failed to connect to ClickHouse at %s:%d: %s", config.host,
		                  static_cast<int32_t>(config.GetPort()), ex.what());
	}
}

void ClickhouseConnection::SetDebugPrintQueries(bool print) {
	debug_print_queries = print;
}

clickhouse::Query ClickhouseConnection::MakeQuery(const string &sql) const {
	clickhouse::Query query(sql);
	query.SetSetting(LOW_CARDINALITY_SETTING, clickhouse::QuerySettingsField {"0", 0});
	for (auto &setting : config.settings) {
		// IMPORTANT makes the server reject unknown settings instead of silently ignoring them
		query.SetSetting(setting.first,
		                 clickhouse::QuerySettingsField {setting.second, clickhouse::QuerySettingsField::IMPORTANT});
	}
	return query;
}

void ClickhouseConnection::RethrowAsDuckDBException(const string &sql) {
	auto query_suffix = debug_print_queries && !sql.empty() ? "\nQuery: " + sql : string();
	try {
		throw;
	} catch (const clickhouse::ServerException &ex) {
		// the server reported an error; the connection itself is still in a clean state
		auto &error = ex.GetException();
		throw IOException("ClickHouse error %d (%s): %s%s", error.code, error.name, error.display_text, query_suffix);
	} catch (const Exception &) {
		broken = true;
		throw;
	} catch (const std::exception &ex) {
		broken = true;
		throw IOException("ClickHouse connection error (%s:%d): %s%s", config.host,
		                  static_cast<int32_t>(config.GetPort()), ex.what(), query_suffix);
	}
}

void ClickhouseConnection::BeginQuery(const string &sql) {
	if (debug_print_queries) {
		Printer::Print(sql + "\n");
	}
	last_used = std::chrono::steady_clock::now();
	try {
		client->BeginSelect(MakeQuery(sql));
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

vector<clickhouse::Block> ClickhouseConnection::Query(const string &sql) {
	vector<clickhouse::Block> result;
	BeginQuery(sql);
	while (true) {
		auto block = NextBlock();
		if (!block) {
			break;
		}
		result.push_back(std::move(*block));
	}
	return result;
}

bool ClickhouseConnection::IsHealthy() {
	if (broken || client->IsSelecting()) {
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
