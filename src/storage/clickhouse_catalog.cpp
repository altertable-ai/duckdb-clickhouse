#include "storage/clickhouse_catalog.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/storage/database_size.hpp"
#include "storage/clickhouse_schema_entry.hpp"

namespace duckdb {

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
	ClickhouseUtils::ThrowReadOnly();
}

void ClickhouseCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	ClickhouseUtils::ThrowReadOnly();
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

PhysicalOperator &ClickhouseCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                       PhysicalOperator &) {
	ClickhouseUtils::ThrowReadOnly();
}

PhysicalOperator &ClickhouseCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
                                                optional_ptr<PhysicalOperator>) {
	ClickhouseUtils::ThrowReadOnly();
}

PhysicalOperator &ClickhouseCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
                                                PhysicalOperator &) {
	ClickhouseUtils::ThrowReadOnly();
}

PhysicalOperator &ClickhouseCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
                                                PhysicalOperator &) {
	ClickhouseUtils::ThrowReadOnly();
}

PhysicalOperator &ClickhouseCatalog::PlanMergeInto(ClientContext &, PhysicalPlanGenerator &, LogicalMergeInto &,
                                                   PhysicalOperator &) {
	// the base Catalog::PlanMergeInto() throws NotImplementedException; MERGE INTO is a write like any
	// other and must be rejected with the same read-only error
	ClickhouseUtils::ThrowReadOnly();
}

unique_ptr<LogicalOperator> ClickhouseCatalog::BindCreateIndex(Binder &, CreateStatement &, TableCatalogEntry &,
                                                               unique_ptr<LogicalOperator>) {
	// must throw here, before the base implementation's IndexBinder::BindCreateIndex() gets anywhere near
	// LogicalGet::bind_data: it assumes bind_data is a TableScanBindData and casts + writes through it,
	// which is type confusion against our ClickhouseScanBindData
	ClickhouseUtils::ThrowReadOnly();
}

unique_ptr<LogicalOperator> ClickhouseCatalog::BindAlterAddIndex(Binder &, TableCatalogEntry &,
                                                                 unique_ptr<LogicalOperator>,
                                                                 unique_ptr<CreateIndexInfo>,
                                                                 unique_ptr<AlterTableInfo>) {
	ClickhouseUtils::ThrowReadOnly();
}

DatabaseSize ClickhouseCatalog::GetDatabaseSize(ClientContext &context) {
	return DatabaseSize();
}

} // namespace duckdb
