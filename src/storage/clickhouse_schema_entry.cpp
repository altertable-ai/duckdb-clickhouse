#include "storage/clickhouse_schema_entry.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"

namespace duckdb {

ClickhouseSchemaEntry::ClickhouseSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info), tables(*this, catalog) {
}

optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &,
                                                              TableCatalogEntry &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
void ClickhouseSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
void ClickhouseSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	ClickhouseUtils::ThrowReadOnly();
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
