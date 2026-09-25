#include "storage/clickhouse_catalog.hpp"

#include "clickhouse_ddl_types.hpp"
#include "clickhouse_utils.hpp"
#include "clickhouse_writer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "storage/clickhouse_ddl.hpp"
#include "storage/clickhouse_insert.hpp"
#include "storage/clickhouse_schema_entry.hpp"
#include "storage/clickhouse_table_entry.hpp"
#include "storage/clickhouse_transaction.hpp"

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

void ClickhouseCatalog::RetireEntries(vector<shared_ptr<CatalogEntry>> entries) {
	GetAttached().GetTransactionManager().Cast<ClickhouseTransactionManager>().RetireEntries(std::move(entries));
}

shared_ptr<CatalogEntry> ClickhouseCatalog::GetSchemaEntryOwner(const string &name) {
	return schemas.GetEntryOwner(name);
}

optional_ptr<CatalogEntry> ClickhouseCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	auto &context = transaction.GetContext();
	string sql = "CREATE DATABASE ";
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		sql += "IF NOT EXISTS ";
	} else if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		throw NotImplementedException("CREATE OR REPLACE SCHEMA is not supported for ClickHouse databases");
	}
	ClickhouseDdl::Execute(context, *this, sql + ClickhouseUtils::QuoteIdentifier(info.schema));
	auto entry = LookupSchema(transaction, EntryLookupInfo(CatalogType::SCHEMA_ENTRY, info.schema),
	                          OnEntryNotFound::RETURN_NULL);
	return entry.get();
}

void ClickhouseCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	auto transaction = GetCatalogTransaction(context);
	auto schema = LookupSchema(transaction, EntryLookupInfo(CatalogType::SCHEMA_ENTRY, info.name),
	                           OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Schema with name \"%s\" does not exist!", info.name);
	}
	auto database = schema->name;
	if (!info.cascade) {
		// DROP DATABASE drops everything in it: match DuckDB, which refuses to drop a non-empty schema without
		// CASCADE
		idx_t table_count = 0;
		{
			auto connection = connection_pool->GetConnection();
			for (auto &block : connection->Query("SELECT count() FROM system.tables WHERE database = " +
			                                     ClickhouseUtils::QuoteLiteral(database))) {
				if (block.GetRowCount() > 0) {
					table_count = block[0]->As<clickhouse::ColumnUInt64>()->At(0);
				}
			}
		}
		if (table_count > 0) {
			throw CatalogException("Cannot drop ClickHouse database \"%s\": it contains %d table(s); use DROP SCHEMA "
			                       "… CASCADE to drop it with everything in it",
			                       database, table_count);
		}
	}
	ClickhouseDdl::Execute(context, *this,
	                       "DROP DATABASE " +
	                           string(info.if_not_found == OnEntryNotFound::RETURN_NULL ? "IF EXISTS " : "") +
	                           ClickhouseUtils::QuoteIdentifier(database));
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

ClickhousePoolConnection ClickhouseCatalog::StartWrite(ClientContext &context) {
	ThrowIfReadOnly();
	auto connection = connection_pool->GetConnection();
	// only once the connection is in hand: if GetConnection() itself throws (e.g. the pool is exhausted), nothing
	// was sent to ClickHouse, so a later ROLLBACK should not warn about an uncommitted write
	ClickhouseTransaction::Get(context, *this).MarkWritten();
	return connection;
}

PhysicalOperator &ClickhouseCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                       LogicalCreateTable &op, PhysicalOperator &plan) {
	ThrowIfReadOnly();
	auto &info = op.info->Base();
	// fail now, before anything is created, for columns ClickHouse cannot hold or that the INSERT cannot write
	ClickhouseDdl::CreateTableSql(context, op.schema.name, info);
	for (auto &column : info.columns.Logical()) {
		auto type = ClickhouseDdlTypes::ToClickhouse(column.Type(), true);
		if (ClickhouseWriter::GetWriteMode(ClickhouseTypeParser::Parse(type)) == ClickhouseWriteMode::UNSUPPORTED) {
			throw NotImplementedException("Column \"%s\" (%s) cannot be written into ClickHouse by CREATE TABLE AS; "
			                              "create the table with clickhouse_execute() instead",
			                              column.Name(), column.Type().ToString());
		}
	}
	auto &insert = planner.Make<ClickhouseInsert>(op, *this, op.schema.name, std::move(op.info));
	insert.children.push_back(plan);
	return insert;
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
