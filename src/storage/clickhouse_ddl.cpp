#include "storage/clickhouse_ddl.hpp"

#include "clickhouse_ddl_types.hpp"
#include "clickhouse_filter_pushdown.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_table_entry.hpp"

namespace duckdb {

//! A constant DEFAULT value as a ClickHouse literal
static string LiteralSql(const Value &value, const string &column_name) {
	if (value.IsNull()) {
		return "NULL";
	}
	// the whole translation -- including the TIMESTAMP family, which itself calls into TransformConstant() and can
	// throw for an infinite timestamp -- is wrapped in one try/catch, so every unsupported DEFAULT (not just the
	// default: branch) gets this DEFAULT-specific message instead of TransformConstant's filter-pushdown wording
	try {
		switch (value.type().id()) {
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE: {
			auto number = value.GetValue<double>();
			if (std::isnan(number)) {
				return "nan";
			}
			if (std::isinf(number)) {
				return number > 0 ? "inf" : "-inf";
			}
			return value.ToString();
		}
		case LogicalTypeId::UUID:
			return "toUUID(" + ClickhouseUtils::QuoteLiteral(value.ToString()) + ")";
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_NS:
			// naive timestamps are stored as UTC (see ClickhouseDdlTypes)
			return ClickhouseFilterPushdown::TransformConstant(
			    Value::TIMESTAMPTZ(timestamp_tz_t(value.DefaultCastAs(LogicalType::TIMESTAMP).GetValue<timestamp_t>())));
		default:
			return ClickhouseFilterPushdown::TransformConstant(value);
		}
	} catch (NotImplementedException &) {
		throw NotImplementedException("DEFAULT value %s of column \"%s\" (type %s) cannot be written as a "
		                              "ClickHouse literal; create the table with clickhouse_execute() instead",
		                              value.ToString(), column_name, value.type().ToString());
	}
}

string ClickhouseDdl::DefaultValueSql(ClientContext &context, const ColumnDefinition &column) {
	if (!column.HasDefaultValue()) {
		return string();
	}
	auto expression = column.DefaultValue().Copy();
	auto binder = Binder::CreateBinder(context);
	ConstantBinder constant_binder(*binder, context, "DEFAULT value");
	// ExpressionBinder::target_type (not Bind()'s "result_type" out-parameter, which only reports back the type
	// actually bound) is what makes Bind() add the cast itself, with the client context available: a plain
	// Value::DefaultCastAs() after the fact has no context and so ignores session settings such as TimeZone
	constant_binder.target_type = column.Type();
	auto bound = constant_binder.Bind(expression);
	// IsFoldable() alone is not enough: now()/current_timestamp/current_database() etc. have
	// FunctionStability::CONSISTENT_WITHIN_QUERY, which DuckDB still considers foldable (it is constant for the
	// lifetime of one query) even though it is not a true constant -- baking today's value into the DDL as a
	// literal DEFAULT would silently stop it from updating on every future INSERT. IsConsistent() additionally
	// requires every function in the tree to be FunctionStability::CONSISTENT, which excludes those.
	if (!bound->IsFoldable() || !bound->IsConsistent()) {
		throw NotImplementedException("DEFAULT value of column \"%s\" must be a constant for ClickHouse tables (got "
		                              "%s); create the table with clickhouse_execute() instead",
		                              column.Name(), column.DefaultValue().ToString());
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, *bound);
	return " DEFAULT " + LiteralSql(value, column.Name());
}

string ClickhouseDdl::ColumnSql(ClientContext &context, const ColumnDefinition &column, bool nullable) {
	if (column.Generated()) {
		throw NotImplementedException("Generated columns are not supported for ClickHouse tables; create the table "
		                              "with clickhouse_execute() instead");
	}
	return ClickhouseUtils::QuoteIdentifier(column.Name()) + " " +
	       ClickhouseDdlTypes::ToClickhouse(column.Type(), nullable) + DefaultValueSql(context, column);
}

void ClickhouseDdl::ValidateEngine(const string &engine) {
	idx_t position = 0;
	auto is_identifier_start = [](char c) { return StringUtil::CharacterIsAlpha(c) || c == '_'; };
	auto is_identifier_char = [](char c) { return StringUtil::CharacterIsAlphaNumeric(c) || c == '_'; };
	bool valid = !engine.empty() && is_identifier_start(engine[0]);
	while (valid && position < engine.size() && is_identifier_char(engine[position])) {
		position++;
	}
	if (valid && position < engine.size()) {
		if (engine[position] != '(') {
			valid = false;
		} else {
			// one parenthesized argument list, balanced and closing exactly at the end of the string. Parens
			// inside a single-quoted string (e.g. a ZooKeeper path argument) do not count, and a backslash
			// escapes the character after it (matching ClickhouseUtils::QuoteLiteral's own escaping), so a
			// quote or backslash inside the string never ends it early
			idx_t depth = 0;
			bool in_string = false;
			for (idx_t i = position; valid && i < engine.size(); i++) {
				char c = engine[i];
				if (in_string) {
					if (c == '\\' && i + 1 < engine.size()) {
						i++;
					} else if (c == '\'') {
						in_string = false;
					}
				} else if (c == '\'') {
					in_string = true;
				} else if (c == '(') {
					depth++;
				} else if (c == ')') {
					if (depth == 0) {
						valid = false;
					} else {
						depth--;
						if (depth == 0 && i != engine.size() - 1) {
							// the group closed before the end of the string (e.g. trailing "ORDER BY (id)")
							valid = false;
						}
					}
				}
			}
			if (in_string || depth != 0) {
				valid = false;
			}
		}
	}
	if (!valid) {
		throw InvalidInputException("Invalid ch_default_table_engine \"%s\": expected an engine name, optionally "
		                            "followed by its arguments, e.g. MergeTree or "
		                            "ReplicatedMergeTree('/clickhouse/tables/{shard}/t', '{replica}')",
		                            engine);
	}
}

string ClickhouseDdl::TableEngine(ClientContext &context) {
	string engine = "MergeTree";
	Value value;
	if (context.TryGetCurrentSetting("ch_default_table_engine", value) && !value.IsNull()) {
		engine = StringValue::Get(value);
	}
	ValidateEngine(engine);
	return engine;
}

//! ORDER BY is required by (and only valid for) the MergeTree family
static bool EngineTakesOrderBy(const string &engine) {
	auto name = engine.substr(0, engine.find('('));
	return StringUtil::EndsWith(name, "MergeTree");
}

string ClickhouseDdl::CreateTableSql(ClientContext &context, const string &database, CreateTableInfo &info) {
	if (!info.partition_keys.empty() || !info.sort_keys.empty() || !info.options.empty()) {
		throw NotImplementedException("PARTITIONED BY, SORTED BY and WITH options are not supported for ClickHouse "
		                              "tables; create the table with clickhouse_execute() instead");
	}
	unordered_set<idx_t> not_null;
	vector<string> primary_key;
	for (auto &constraint : info.constraints) {
		switch (constraint->type) {
		case ConstraintType::NOT_NULL:
			not_null.insert(constraint->Cast<NotNullConstraint>().index.index);
			break;
		case ConstraintType::UNIQUE: {
			auto &unique = constraint->Cast<UniqueConstraint>();
			if (!unique.IsPrimaryKey()) {
				throw NotImplementedException("UNIQUE constraints are not supported for ClickHouse tables");
			}
			if (unique.HasIndex()) {
				primary_key.push_back(info.columns.GetColumn(unique.GetIndex()).Name());
			} else {
				primary_key = unique.GetColumnNames();
			}
			break;
		}
		case ConstraintType::CHECK:
			throw NotImplementedException("CHECK constraints are not supported for ClickHouse tables");
		case ConstraintType::FOREIGN_KEY:
			throw NotImplementedException("FOREIGN KEY constraints are not supported for ClickHouse tables");
		default:
			throw NotImplementedException("This constraint is not supported for ClickHouse tables");
		}
	}
	vector<string> columns;
	for (auto &column : info.columns.Logical()) {
		bool in_primary_key = false;
		for (auto &key : primary_key) {
			in_primary_key = in_primary_key || StringUtil::CIEquals(key, column.Name());
		}
		auto nullable = !in_primary_key && not_null.find(column.Logical().index) == not_null.end();
		columns.push_back(ColumnSql(context, column, nullable));
	}
	auto engine = TableEngine(context);
	string sql = "CREATE ";
	if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		sql += "OR REPLACE ";
	}
	sql += "TABLE ";
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		sql += "IF NOT EXISTS ";
	}
	sql += ClickhouseUtils::QuoteIdentifier(database) + "." + ClickhouseUtils::QuoteIdentifier(info.table) + " (" +
	       StringUtil::Join(columns, ", ") + ") ENGINE = " + engine;
	if (EngineTakesOrderBy(engine)) {
		if (primary_key.empty()) {
			sql += " ORDER BY tuple()";
		} else {
			vector<string> keys;
			for (auto &key : primary_key) {
				keys.push_back(ClickhouseUtils::QuoteIdentifier(info.columns.GetColumn(key).Name()));
			}
			sql += " ORDER BY (" + StringUtil::Join(keys, ", ") + ")";
		}
	}
	return sql;
}

void ClickhouseDdl::Execute(ClientContext &context, ClickhouseCatalog &catalog, const string &sql) {
	try {
		auto connection = catalog.StartWrite(context);
		// Time/Time64 columns need this setting (ClickHouse 25.x); servers that do not know it ignore it
		connection->Execute(sql, {{"enable_time_time64_type", "1"}});
	} catch (...) {
		// a failed statement can still have changed something
		catalog.ClearCache();
		throw;
	}
	catalog.ClearCache();
}

optional_ptr<ClickhouseTableEntry> ClickhouseDdl::LookupTable(ClientContext &context, ClickhouseCatalog &catalog,
                                                              const string &database, const string &table) {
	auto transaction = catalog.GetCatalogTransaction(context);
	auto schema = catalog.LookupSchema(transaction, EntryLookupInfo(CatalogType::SCHEMA_ENTRY, database),
	                                   OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		return nullptr;
	}
	auto entry = schema->LookupEntry(transaction, EntryLookupInfo(CatalogType::TABLE_ENTRY, table));
	if (!entry) {
		return nullptr;
	}
	return &entry->Cast<ClickhouseTableEntry>();
}

ClickhouseTableEntry &ClickhouseDdl::CreateTable(ClientContext &context, ClickhouseCatalog &catalog,
                                                 const string &database, CreateTableInfo &info) {
	Execute(context, catalog, CreateTableSql(context, database, info));
	auto entry = LookupTable(context, catalog, database, info.table);
	if (!entry) {
		throw InvalidInputException("ClickHouse table \"%s\".\"%s\" was created but cannot be found; run CALL "
		                            "clickhouse_clear_cache() and retry",
		                            database, info.table);
	}
	return *entry;
}

string ClickhouseDdl::AlterTableSql(ClientContext &context, const string &database, const string &table,
                                    AlterTableInfo &info) {
	auto qualified = ClickhouseUtils::QuoteIdentifier(database) + "." + ClickhouseUtils::QuoteIdentifier(table);
	switch (info.alter_table_type) {
	case AlterTableType::ADD_COLUMN: {
		auto &add = info.Cast<AddColumnInfo>();
		// added columns are always Nullable: DuckDB's ADD COLUMN carries no NOT NULL constraint
		return "ALTER TABLE " + qualified + " ADD COLUMN " + (add.if_column_not_exists ? "IF NOT EXISTS " : "") +
		       ColumnSql(context, add.new_column, true);
	}
	case AlterTableType::REMOVE_COLUMN: {
		auto &remove = info.Cast<RemoveColumnInfo>();
		return "ALTER TABLE " + qualified + " DROP COLUMN " + (remove.if_column_exists ? "IF EXISTS " : "") +
		       ClickhouseUtils::QuoteIdentifier(remove.removed_column);
	}
	case AlterTableType::RENAME_COLUMN: {
		auto &rename = info.Cast<RenameColumnInfo>();
		return "ALTER TABLE " + qualified + " RENAME COLUMN " + ClickhouseUtils::QuoteIdentifier(rename.old_name) +
		       " TO " + ClickhouseUtils::QuoteIdentifier(rename.new_name);
	}
	case AlterTableType::RENAME_TABLE: {
		auto &rename = info.Cast<RenameTableInfo>();
		return "RENAME TABLE " + qualified + " TO " + ClickhouseUtils::QuoteIdentifier(database) + "." +
		       ClickhouseUtils::QuoteIdentifier(rename.new_table_name);
	}
	default:
		throw NotImplementedException("This ALTER TABLE operation is not supported for ClickHouse tables (only ADD "
		                              "COLUMN, DROP COLUMN, RENAME COLUMN and RENAME TO are); run it with "
		                              "clickhouse_execute() instead");
	}
}

} // namespace duckdb
