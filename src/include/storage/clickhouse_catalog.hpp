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

	ClickhouseConnectionConfig config;
	ClickhouseAttachOptions options;
	shared_ptr<ClickhouseConnectionPool> connection_pool;
	ClickhouseSchemaSet schemas;
};

} // namespace duckdb
