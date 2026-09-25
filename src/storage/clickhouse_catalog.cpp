#include "storage/clickhouse_catalog.hpp"

#include "clickhouse_ddl_types.hpp"
#include "clickhouse_utils.hpp"
#include "clickhouse_writer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"
#include "duckdb/planner/operator/logical_create_table.hpp"
#include "duckdb/planner/operator/logical_delete.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"
#include "duckdb/planner/operator/logical_update.hpp"
#include "duckdb/planner/parsed_data/bound_create_table_info.hpp"
#include "duckdb/storage/database_size.hpp"
#include "storage/clickhouse_ddl.hpp"
#include "storage/clickhouse_dml.hpp"
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

//! The real name of the database the SCHEMA option `name` refers to, by the rule ClickhouseCatalogSet::GetEntry()
//! applies to schema names: the exact name, else the first case-insensitive match in name order (ClickHouse's lower()
//! is ASCII-only, like StringUtil::CIEquals())
static string ResolveSchemaOption(ClickhousePoolConnection &connection, const string &name) {
	string match;
	for (auto &block : connection->Query("SELECT name FROM system.databases WHERE lower(name) = lower(" +
	                                     ClickhouseUtils::QuoteLiteral(name) + ") ORDER BY name")) {
		auto names = block[0]->As<clickhouse::ColumnString>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			string candidate(names->At(row));
			if (candidate == name) {
				return candidate;
			}
			if (match.empty()) {
				match = candidate;
			}
		}
	}
	if (match.empty()) {
		throw BinderException("ClickHouse database \"%s\" (the SCHEMA option of ATTACH) does not exist", name);
	}
	return match;
}

ClickhouseCatalog::ClickhouseCatalog(AttachedDatabase &db, ClickhouseConnectionConfig config_p,
                                     ClickhouseAttachOptions options_p, ClientContext &context)
    : Catalog(db), config(std::move(config_p)), options(options_p),
      connection_pool(make_shared_ptr<ClickhouseConnectionPool>(
          config, ClickhouseTimeouts::FromContext(context), ClickhouseConnectionPool::PoolConfigFromContext(context))),
      schemas(*this) {
	// connect now so that a wrong host, port, certificate or password fails the ATTACH itself
	auto connection = connection_pool->GetConnection();
	if (!options.schema.empty()) {
		// fail the ATTACH, too, for a SCHEMA that does not exist
		options.schema = ResolveSchemaOption(connection, options.schema);
	}
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

//! "main" is not a ClickHouse database name here: LookupSchema() maps it to GetDefaultSchema(), so DROP SCHEMA
//! ch.main would drop that database (and then trip DuckDB's own PhysicalDrop assertion that "main" is never dropped),
//! and CREATE SCHEMA ch.main would create a real `main` database that silently takes over what ch.main means from
//! then on (any case: LookupSchema() also finds a `MAIN` database case-insensitively)
void ClickhouseCatalog::ThrowIfDefaultSchema(const string &schema_name) const {
	if (!StringUtil::CIEquals(schema_name, DEFAULT_SCHEMA)) {
		return;
	}
	if (!options.schema.empty()) {
		throw CatalogException("Cannot create or drop schema \"%s\": in this attached ClickHouse database it stands "
		                       "for the SCHEMA database (\"%s\"); use its own name instead",
		                       schema_name, options.schema);
	}
	throw CatalogException("Cannot create or drop schema \"%s\": in an attached ClickHouse database it stands for "
	                       "the connection's database (\"%s\"); use clickhouse_execute() to manage a ClickHouse "
	                       "database with that name",
	                       schema_name, config.database);
}

optional_ptr<CatalogEntry> ClickhouseCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	auto &context = transaction.GetContext();
	ThrowIfDefaultSchema(info.schema);
	if (!options.schema.empty()) {
		if (!StringUtil::CIEquals(info.schema, options.schema)) {
			throw CatalogException("Cannot create schema \"%s\": \"%s\" was attached with SCHEMA '%s' and shows no "
			                       "other ClickHouse database; use clickhouse_execute() to create it",
			                       info.schema, GetName(), options.schema);
		}
		// the one name the schema list shows
		info.schema = options.schema;
	}
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
	ThrowIfDefaultSchema(info.name);
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
			                                         ClickhouseUtils::QuoteLiteral(database),
			                                     ClickhouseDml::SemanticSettings())) {
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
		// "main" refers to the SCHEMA database, else to the database named in the connection settings
		entry = schemas.GetEntry(context, GetDefaultSchema());
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
	// only the owned CreateTableInfo is kept: BoundCreateTableInfo::schema is a reference into this catalog's cache
	auto create_info = unique_ptr_cast<CreateInfo, CreateTableInfo>(std::move(op.info->base));
	auto &insert = planner.Make<ClickhouseInsert>(op, *this, op.schema.name, std::move(create_info));
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

PhysicalOperator &ClickhouseCatalog::PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner,
                                                LogicalDelete &op) {
	ThrowIfReadOnly();
	return planner.Make<ClickhouseDmlOperator>(op, ClickhouseDml::PlanDelete(op));
}

//! DuckDB v1.5.4 calls the overloads taking an already planned child only from the default implementations of the
//! logical-level PlanDelete/PlanUpdate, which ClickhouseCatalog overrides: never expected. A planned child cannot be
//! translated into ClickHouse SQL, so the statement is refused (not an InternalException, which would invalidate
//! the database instance)
[[noreturn]] static void ThrowPlannedChild(const string &statement) {
	throw NotImplementedException("%s on ClickHouse tables is translated from its logical plan only, and this plan "
	                              "reached the extension already planned; run the statement with clickhouse_execute() "
	                              "instead",
	                              statement);
}

PhysicalOperator &ClickhouseCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
                                                PhysicalOperator &) {
	ThrowIfReadOnly();
	ThrowPlannedChild("DELETE");
}

PhysicalOperator &ClickhouseCatalog::PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner,
                                                LogicalUpdate &op) {
	ThrowIfReadOnly();
	return planner.Make<ClickhouseDmlOperator>(op, ClickhouseDml::PlanUpdate(context, op));
}

PhysicalOperator &ClickhouseCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
                                                PhysicalOperator &) {
	ThrowIfReadOnly();
	ThrowPlannedChild("UPDATE");
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
