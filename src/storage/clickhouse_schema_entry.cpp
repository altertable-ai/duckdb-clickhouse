#include "storage/clickhouse_schema_entry.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {

[[noreturn]] static void ThrowNotSupported(const string &what) {
	throw NotImplementedException("%s cannot be created in a ClickHouse database", what);
}

ClickhouseSchemaEntry::ClickhouseSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info), tables(*this, catalog) {
}

optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	ClickhouseUtils::ThrowUnsupportedWrite("CREATE TABLE");
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	ThrowNotSupported("Functions and macros");
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &,
                                                              TableCatalogEntry &) {
	throw NotImplementedException("Indexes are not supported for ClickHouse tables");
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	throw NotImplementedException(
	    "CREATE VIEW is not supported for ClickHouse databases; create the view in ClickHouse with "
	    "clickhouse_execute()");
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	ThrowNotSupported("Sequences");
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	ThrowNotSupported("Table functions");
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	ThrowNotSupported("Copy functions");
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
	ThrowNotSupported("Pragma functions");
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	ThrowNotSupported("Collations");
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	ThrowNotSupported("Types");
}
void ClickhouseSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	ClickhouseUtils::ThrowUnsupportedWrite("ALTER TABLE");
}
void ClickhouseSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	ClickhouseUtils::ThrowUnsupportedWrite("DROP");
}

void ClickhouseSchemaEntry::Scan(ClientContext &context, CatalogType type,
                                 const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	tables.Scan(context, callback);
}

void ClickhouseSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw NotImplementedException("Scanning a ClickHouse schema requires a client context");
}

optional_ptr<CatalogEntry> ClickhouseSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                              const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return tables.GetEntry(transaction.GetContext(), lookup_info.GetEntryName());
}

} // namespace duckdb
