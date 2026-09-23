#define DUCKDB_EXTENSION_MAIN

#include "clickhouse_scanner_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <clickhouse/client.h>

namespace duckdb {

static void ClickhouseClientVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto version = clickhouse::Client::GetVersion();
	auto text = StringUtil::Format("%d.%d.%d", version.major, version.minor, version.patch);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, text);
}

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunction version_function("clickhouse_client_version", {}, LogicalType::VARCHAR,
	                                ClickhouseClientVersionFunction);
	loader.RegisterFunction(version_function);
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
