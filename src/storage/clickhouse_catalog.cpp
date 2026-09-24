#include "storage/clickhouse_catalog.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/storage/database_size.hpp"
#include "storage/clickhouse_insert.hpp"
#include "storage/clickhouse_schema_entry.hpp"
#include "storage/clickhouse_table_entry.hpp"

namespace duckdb {

ClickhouseCatalog &ClickhouseCatalog::GetAttachedDatabase(ClientContext &context, const string &database_name,
                                                           const string &function_name) {
	auto database = DatabaseManager::Get(context).GetDatabase(context, database_name);
	if (!database) {
		throw BinderException("Failed to find attached database \"%s\" referenced in %s", database_name,
		                      function_name);
	}
	auto &catalog = database->GetCatalog();
	if (catalog.GetCatalogType() != CATALOG_TYPE) {
		throw BinderException("Attached database \"%s\" is not a ClickHouse database", database_name);
	}
	return catalog.Cast<ClickhouseCatalog>();
}

ClickhouseCatalog::ClickhouseCatalog(AttachedDatabase &db, ClickhouseConnectionConfig config_p,
                                     ClickhouseAttachOptions options_p, ClientContext &context)
    : Catalog(db), config(std::move(config_p)), options(options_p),
      connection_pool(make_shared_ptr<ClickhouseConnectionPool>(
          config, ClickhouseTimeouts::FromContext(context), ClickhouseConnectionPool::PoolConfigFromContext(context))),
      schemas(*this) {
	// connect now so that a wrong host, port, certificate or password fails the ATTACH itself
	auto connection = connection_pool->GetConnection();
}

ClickhouseCatalog::~ClickhouseCatalog() = default;

void ClickhouseCatalog::Initialize(bool load_builtin) {
}

void ClickhouseCatalog::ClearCache() {
	schemas.ClearEntries();
}

shared_ptr<CatalogEntry> ClickhouseCatalog::GetSchemaEntryOwner(const string &name) {
	return schemas.GetEntryOwner(name);
}

optional_ptr<CatalogEntry> ClickhouseCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	ClickhouseUtils::ThrowUnsupportedWrite("CREATE SCHEMA");
}

void ClickhouseCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	ClickhouseUtils::ThrowUnsupportedWrite("DROP SCHEMA");
}

void ClickhouseCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	schemas.Scan(context, [&](CatalogEntry &schema) { callback(schema.Cast<SchemaCatalogEntry>()); });
}

optional_ptr<SchemaCatalogEntry> ClickhouseCatalog::LookupSchema(CatalogTransaction transaction,
                                                                 const EntryLookupInfo &schema_lookup,
                                                                 OnEntryNotFound if_not_found) {
	auto &context = transaction.GetContext();
	auto &schema_name = schema_lookup.GetEntryName();
	auto entry = schemas.GetEntry(context, schema_name);
	if (!entry && schema_name == DEFAULT_SCHEMA) {
		// "main" refers to the database named in the connection settings
		entry = schemas.GetEntry(context, config.database);
	}
	if (!entry) {
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			return nullptr;
		}
		throw BinderException("ClickHouse database \"%s\" not found", schema_name);
	}
	return &entry->Cast<SchemaCatalogEntry>();
}

void ClickhouseCatalog::ThrowIfReadOnly() const {
	if (GetAttached().IsReadOnly()) {
		ClickhouseUtils::ThrowReadOnly(GetName());
	}
}

PhysicalOperator &ClickhouseCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                       PhysicalOperator &) {
	ThrowIfReadOnly();
	ClickhouseUtils::ThrowUnsupportedWrite("CREATE TABLE AS");
}

PhysicalOperator &ClickhouseCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner,
                                                LogicalInsert &op, optional_ptr<PhysicalOperator> plan) {
	ThrowIfReadOnly();
	if (op.return_chunk) {
		throw NotImplementedException("RETURNING is not supported for ClickHouse tables");
	}
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		throw NotImplementedException("ON CONFLICT is not supported for ClickHouse tables");
	}
	D_ASSERT(plan);
	// DuckDB's own planner adds a projection filling in the DEFAULTs of unlisted columns
	// (ResolveDefaultsProjection); it is skipped on purpose, so ClickHouse applies its own defaults instead
	auto &table = op.table.Cast<ClickhouseTableEntry>();
	auto columns = ClickhouseInsert::GetInsertColumns(table, op.column_index_map);
	auto &insert = planner.Make<ClickhouseInsert>(op, table, std::move(columns));
	insert.children.push_back(*plan);
	return insert;
}

PhysicalOperator &ClickhouseCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
                                                PhysicalOperator &) {
	ThrowIfReadOnly();
	ClickhouseUtils::ThrowUnsupportedWrite("DELETE");
}

PhysicalOperator &ClickhouseCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
                                                PhysicalOperator &) {
	ThrowIfReadOnly();
	ClickhouseUtils::ThrowUnsupportedWrite("UPDATE");
}

PhysicalOperator &ClickhouseCatalog::PlanMergeInto(ClientContext &, PhysicalPlanGenerator &, LogicalMergeInto &,
                                                   PhysicalOperator &) {
	ThrowIfReadOnly();
	throw NotImplementedException("MERGE INTO is not supported for ClickHouse tables");
}

unique_ptr<LogicalOperator> ClickhouseCatalog::BindCreateIndex(Binder &, CreateStatement &, TableCatalogEntry &,
                                                               unique_ptr<LogicalOperator>) {
	// must throw here, before the base implementation's IndexBinder::BindCreateIndex() gets anywhere near
	// LogicalGet::bind_data: it assumes bind_data is a TableScanBindData and casts + writes through it,
	// which is type confusion against our ClickhouseScanBindData
	throw NotImplementedException("Indexes are not supported for ClickHouse tables");
}

unique_ptr<LogicalOperator> ClickhouseCatalog::BindAlterAddIndex(Binder &, TableCatalogEntry &,
                                                                 unique_ptr<LogicalOperator>,
                                                                 unique_ptr<CreateIndexInfo>,
                                                                 unique_ptr<AlterTableInfo>) {
	throw NotImplementedException("Indexes are not supported for ClickHouse tables");
}

DatabaseSize ClickhouseCatalog::GetDatabaseSize(ClientContext &context) {
	return DatabaseSize();
}

} // namespace duckdb
