#include "storage/clickhouse_schema_entry.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_ddl.hpp"
#include "storage/clickhouse_table_entry.hpp"

namespace duckdb {

[[noreturn]] static void ThrowNotSupported(const string &what) {
	throw NotImplementedException("%s cannot be created in a ClickHouse database", what);
}

ClickhouseSchemaEntry::ClickhouseSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info), tables(*this, catalog) {
}

optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                               BoundCreateTableInfo &info) {
	auto &context = transaction.GetContext();
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	// ClickhouseDdl clears the catalog cache, which frees cached schema entries -- including this one
	auto keep_alive = ch_catalog.GetSchemaEntryOwner(name);
	return &ClickhouseDdl::CreateTable(context, ch_catalog, name, info.Base());
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

//! ClickHouse table engines that are not tables: DROP TABLE refuses them
static bool IsViewLikeEngine(const string &engine) {
	return engine == "View" || engine == "MaterializedView" || engine == "LiveView" || engine == "WindowView" ||
	       engine == "Dictionary";
}

void ClickhouseSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	if (info.type != CatalogType::TABLE_ENTRY) {
		throw NotImplementedException("DROP %s is not supported for ClickHouse databases",
		                              CatalogTypeToString(info.type));
	}
	auto entry = tables.GetEntry(context, info.name);
	if (!entry) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Table with name %s does not exist!", info.name);
	}
	auto &table = entry->Cast<ClickhouseTableEntry>();
	if (IsViewLikeEngine(table.GetEngine())) {
		throw NotImplementedException("\"%s\" is a ClickHouse view (engine %s), which DROP TABLE does not drop; "
		                              "drop it with clickhouse_execute('%s', 'DROP VIEW %s.%s') instead",
		                              table.name, table.GetEngine(), catalog.GetName(), name, table.name);
	}
	auto sql = "DROP TABLE " + string(info.if_not_found == OnEntryNotFound::RETURN_NULL ? "IF EXISTS " : "") +
	           ClickhouseUtils::QuoteIdentifier(name) + "." + ClickhouseUtils::QuoteIdentifier(table.name);
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	auto keep_alive = ch_catalog.GetSchemaEntryOwner(name);
	// `table` is freed by the cache clear inside Execute(): not used after this point
	ClickhouseDdl::Execute(context, ch_catalog, sql);
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
