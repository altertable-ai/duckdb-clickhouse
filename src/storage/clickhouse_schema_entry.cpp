#include "storage/clickhouse_schema_entry.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_ddl.hpp"
#include "storage/clickhouse_table_entry.hpp"

namespace duckdb {

[[noreturn]] static void ThrowNotSupported(const string &what) {
	throw NotImplementedException("%s cannot be created in a ClickHouse database", what);
}

//! ClickHouse table engines that are not tables: DROP TABLE and ALTER TABLE refuse them
static bool IsViewLikeEngine(const string &engine) {
	return engine == "View" || engine == "MaterializedView" || engine == "LiveView" || engine == "WindowView" ||
	       engine == "Dictionary";
}

//! "dictionary" for engine Dictionary, "view" for the rest of IsViewLikeEngine's engines -- shared error-message
//! wording between DropEntry and Alter
static const char *ViewLikeKind(const string &engine) {
	return engine == "Dictionary" ? "dictionary" : "view";
}

ClickhouseSchemaEntry::ClickhouseSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info), tables(*this, catalog) {
}

optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                               BoundCreateTableInfo &info) {
	auto &context = transaction.GetContext();
	auto &base = info.Base();
	if (base.on_conflict != OnCreateConflict::REPLACE_ON_CONFLICT) {
		// respect the planner's view of the table. PhysicalPlanGenerator::CreatePlan(LogicalCreateTable) routes a
		// CREATE TABLE … AS SELECT here -- dropping its SELECT -- whenever this cache already has the table (and
		// only reaches PlanCreateTableAs otherwise). If the table was dropped on the server behind this cache's back,
		// sending a plain CREATE TABLE would succeed and silently leave an empty table instead of the query's rows
		auto existing = tables.GetEntry(context, base.table);
		if (existing) {
			if (base.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
				return existing;
			}
			throw CatalogException("Table \"%s\" already exists in ClickHouse database \"%s\" (if it was dropped "
			                       "outside DuckDB, run CALL clickhouse_clear_cache())",
			                       base.table, name);
		}
	}
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	// ClickhouseDdl clears the catalog cache, which retires this schema entry; retirement keeps it alive until this
	// transaction ends (see ClickhouseTransactionManager), and keep_alive does too (belt-and-braces)
	auto keep_alive = ch_catalog.GetSchemaEntryOwner(name);
	return &ClickhouseDdl::CreateTable(context, ch_catalog, name, base);
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
void ClickhouseSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	if (info.type != AlterType::ALTER_TABLE) {
		throw NotImplementedException("This ALTER statement is not supported for ClickHouse databases; run it with "
		                              "clickhouse_execute() instead");
	}
	auto &context = transaction.GetContext();
	auto &alter = info.Cast<AlterTableInfo>();
	auto entry = tables.GetEntry(context, alter.name);
	if (!entry) {
		if (alter.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Table with name %s does not exist!", alter.name);
	}
	auto &table = entry->Cast<ClickhouseTableEntry>();
	if (IsViewLikeEngine(table.GetEngine())) {
		throw NotImplementedException(
		    "\"%s\" is a ClickHouse %s (engine %s), which ALTER TABLE cannot modify; alter it with "
		    "clickhouse_execute() instead",
		    table.name, ViewLikeKind(table.GetEngine()), table.GetEngine());
	}
	// built before Execute() clears the cache and retires `entry`
	auto sql = ClickhouseDdl::AlterTableSql(context, name, table, alter);
	if (sql.empty()) {
		// ADD COLUMN IF NOT EXISTS of an existing column, DROP COLUMN IF EXISTS of a missing one
		return;
	}
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	auto keep_alive = ch_catalog.GetSchemaEntryOwner(name);
	ClickhouseDdl::Execute(context, ch_catalog, sql);
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
		auto kind = ViewLikeKind(table.GetEngine());
		auto drop_keyword = table.GetEngine() == "Dictionary" ? "DICTIONARY" : "VIEW";
		throw NotImplementedException(
		    "\"%s\" is a ClickHouse %s (engine %s), which DROP TABLE does not drop; drop it with "
		    "clickhouse_execute('%s', 'DROP %s %s') instead",
		    table.name, kind, table.GetEngine(), catalog.GetName(), drop_keyword,
		    ClickhouseUtils::QualifiedName(name, table.name));
	}
	auto sql = "DROP TABLE " + string(info.if_not_found == OnEntryNotFound::RETURN_NULL ? "IF EXISTS " : "") +
	           ClickhouseUtils::QualifiedName(name, table.name);
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	// belt-and-braces: retirement already keeps `this` alive until this transaction ends
	auto keep_alive = ch_catalog.GetSchemaEntryOwner(name);
	// the cache clear inside Execute() retires `table`: not used after this point
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
