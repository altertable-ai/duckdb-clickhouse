#pragma once

#include "clickhouse_connection_config.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "storage/clickhouse_connection_pool.hpp"
#include "storage/clickhouse_schema_set.hpp"

namespace duckdb {

struct ClickhouseAttachOptions {
	//! Also expose system, INFORMATION_SCHEMA and information_schema
	bool show_system = false;
};

class ClickhouseCatalog : public Catalog {
public:
	ClickhouseCatalog(AttachedDatabase &db, ClickhouseConnectionConfig config, ClickhouseAttachOptions options,
	                  ClientContext &context);
	~ClickhouseCatalog() override;

	static constexpr const char *CATALOG_TYPE = "clickhouse";

	//! The ClickHouse catalog attached as `database_name`. Throws BinderException("Failed to find attached
	//! database \"%s\" referenced in <function_name>") if no such database is attached, or
	//! BinderException("Attached database \"%s\" is not a ClickHouse database") if it is attached through some
	//! other extension. Callers that need to reject a read-only attach do so on top, with
	//! GetAttached().IsReadOnly(). Named GetAttachedDatabase rather than the base class's GetAttached() --
	//! a static overload of that name would hide every inherited overload of it, including the one this
	//! function itself and ThrowIfReadOnly() rely on.
	static ClickhouseCatalog &GetAttachedDatabase(ClientContext &context, const string &database_name,
	                                              const string &function_name);

	const ClickhouseConnectionConfig &GetConfig() const {
		return config;
	}
	const ClickhouseAttachOptions &GetAttachOptions() const {
		return options;
	}
	ClickhouseConnectionPool &GetConnectionPool() {
		return *connection_pool;
	}
	shared_ptr<ClickhouseConnectionPool> GetConnectionPoolPtr() {
		return connection_pool;
	}
	//! Forgets all cached databases, tables and columns
	void ClearCache();
	//! The shared_ptr owning the cached schema entry for the ClickHouse database `name`; null once the
	//! cache has been cleared. Lets a bound scan keep its schema -- and therefore the table entry that
	//! schema's table set owns -- alive past a ClearCache() (see ClickhouseScanBindData::lifetime).
	shared_ptr<CatalogEntry> GetSchemaEntryOwner(const string &name);

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return CATALOG_TYPE;
	}
	string GetDefaultSchema() const override {
		return config.database;
	}
	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanMergeInto(ClientContext &context, PhysicalPlanGenerator &planner, LogicalMergeInto &op,
	                                PhysicalOperator &plan) override;
	//! The base Catalog::BindCreateIndex() would otherwise bind straight into IndexBinder::BindCreateIndex(),
	//! which assumes bind_data is a TableScanBindData and writes through it -- type confusion against our
	//! ClickhouseScanBindData. Throwing here, before anything touches the bind data, is required, not just a
	//! nicer error message.
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;
	unique_ptr<LogicalOperator> BindAlterAddIndex(Binder &binder, TableCatalogEntry &table_entry,
	                                              unique_ptr<LogicalOperator> plan,
	                                              unique_ptr<CreateIndexInfo> create_info,
	                                              unique_ptr<AlterTableInfo> alter_info) override;
	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return config.ToDisplayString();
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;
	//! Throws ClickhouseUtils::ThrowReadOnly() if this database was attached with READ_ONLY. DuckDB's own
	//! read-only check runs only after physical planning; PlanInsert/PlanDelete/PlanUpdate/PlanCreateTableAs/
	//! PlanMergeInto are invoked during physical planning itself, so DuckDB's check never gets a chance to run
	//! for them and this extension must check first.
	void ThrowIfReadOnly() const;

	ClickhouseConnectionConfig config;
	ClickhouseAttachOptions options;
	shared_ptr<ClickhouseConnectionPool> connection_pool;
	ClickhouseSchemaSet schemas;
};

} // namespace duckdb
