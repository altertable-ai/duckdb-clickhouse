#include "storage/clickhouse_ddl.hpp"

#include "clickhouse_ddl_types.hpp"
#include "clickhouse_expression.hpp"
#include "clickhouse_types.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/constraints/list.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_dml.hpp"
#include "storage/clickhouse_table_entry.hpp"

namespace duckdb {

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
		                              "%s); use clickhouse_execute() instead",
		                              column.Name(), column.DefaultValue().ToString());
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, *bound);
	// every unsupported DEFAULT -- including an infinite timestamp, which TransformConstant() refuses -- gets this
	// DEFAULT-specific message instead of the literal writer's own wording
	try {
		return " DEFAULT " + ClickhouseExpression::Literal(value);
	} catch (NotImplementedException &) {
		throw NotImplementedException("DEFAULT value %s of column \"%s\" (type %s) cannot be written as a "
		                              "ClickHouse literal; use clickhouse_execute() instead",
		                              value.ToString(), column.Name(), value.type().ToString());
	}
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
	sql += ClickhouseUtils::QualifiedName(database, info.table) + " (" + StringUtil::Join(columns, ", ") +
	       ") ENGINE = " + engine;
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

void ClickhouseDdl::Execute(ClientContext &context, ClickhouseCatalog &catalog, const string &sql,
                            const vector<std::pair<string, string>> &settings) {
	try {
		auto connection = catalog.StartWrite(context);
		// Time/Time64 columns need this setting (ClickHouse 25.x); servers that do not know it ignore it
		vector<std::pair<string, string>> query_settings {{"enable_time_time64_type", "1"}};
		query_settings.insert(query_settings.end(), settings.begin(), settings.end());
		connection->Execute(sql, query_settings);
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

optional_ptr<const ClickhouseColumnInfo> ClickhouseDdl::ResolveColumn(const ClickhouseTableEntry &table,
                                                                      const string &name) {
	optional_ptr<const ClickhouseColumnInfo> result;
	idx_t case_insensitive_matches = 0;
	for (auto &column : table.GetClickhouseColumns()) {
		if (column.name == name) {
			return &column;
		}
		if (StringUtil::CIEquals(column.name, name)) {
			result = &column;
			case_insensitive_matches++;
		}
	}
	if (case_insensitive_matches > 1) {
		throw CatalogException("Column name \"%s\" is ambiguous in ClickHouse table \"%s\": several columns match it "
		                       "case-insensitively; quote the exact ClickHouse column name",
		                       name, table.name);
	}
	return result;
}

//! " (ClickHouse column \"ID\")" when `column` is spelled differently from the name the statement used
static string SpelledAs(const ClickhouseColumnInfo &column, const string &name) {
	return column.name == name ? string() : " (ClickHouse column \"" + column.name + "\")";
}

static const ClickhouseColumnInfo &RequireColumn(const ClickhouseTableEntry &table, const string &name) {
	auto column = ClickhouseDdl::ResolveColumn(table, name);
	if (!column) {
		throw CatalogException("Column with name \"%s\" does not exist in ClickHouse table \"%s\"", name, table.name);
	}
	return *column;
}

//! A column as the server has it now: the cached entry may be stale, and a MODIFY COLUMN built from a stale type
//! would change more than asked
struct ClickhouseServerColumn {
	ClickhouseColumnInfo info;
	//! system.columns.default_kind: "", DEFAULT, MATERIALIZED, ALIAS or EPHEMERAL
	string default_kind;
	//! Part of the sorting, primary, partition or sampling key
	bool in_key = false;
};

static ClickhouseServerColumn LoadServerColumn(ClickhouseConnection &connection, const string &database,
                                               const ClickhouseTableEntry &table, const string &column) {
	auto sql = "SELECT type, default_kind, is_in_sorting_key OR is_in_primary_key OR is_in_partition_key OR "
	           "is_in_sampling_key FROM system.columns WHERE database = " +
	           ClickhouseUtils::QuoteLiteral(database) + " AND table = " + ClickhouseUtils::QuoteLiteral(table.name) +
	           " AND name = " + ClickhouseUtils::QuoteLiteral(column);
	for (auto &block : connection.Query(sql, ClickhouseConnection::ExtensionQuerySettings())) {
		if (block.GetRowCount() == 0) {
			continue;
		}
		ClickhouseServerColumn result;
		result.info = ClickhouseColumnInfo::Create(column, string(block[0]->As<clickhouse::ColumnString>()->At(0)));
		result.default_kind = string(block[1]->As<clickhouse::ColumnString>()->At(0));
		result.in_key = block[2]->As<clickhouse::ColumnUInt8>()->At(0) != 0;
		return result;
	}
	throw CatalogException("Column \"%s\" of ClickHouse table \"%s\" no longer exists; run CALL "
	                       "clickhouse_clear_cache() and retry",
	                       column, table.name);
}

//! Runs with ClickhouseConnection::ExtensionQuerySettings() plus `extra_settings`: the count reads every stored row
//! that is not deleted, whatever the ATTACH's settings= say
static uint64_t QueryCount(ClickhouseConnection &connection, const string &sql,
                           const vector<std::pair<string, string>> &extra_settings = {}) {
	auto settings = ClickhouseConnection::ExtensionQuerySettings();
	settings.insert(settings.end(), extra_settings.begin(), extra_settings.end());
	uint64_t count = 0;
	for (auto &block : connection.Query(sql, settings)) {
		if (block.GetRowCount() > 0) {
			count = block[0]->As<clickhouse::ColumnUInt64>()->At(0);
		}
	}
	return count;
}

//! How many rows of `qualified` satisfy `condition`, counting the rows a lightweight DELETE only masked: a mutation
//! still rewrites (and converts) them. countIf, not WHERE: ClickHouse 25.8 answers WHERE n IS NULL with 0 for masked
//! rows even with apply_deleted_mask = 0
static uint64_t CountStoredRows(ClickhouseConnection &connection, const string &qualified, const string &condition) {
	return QueryCount(connection, "SELECT countIf(" + condition + ") FROM " + qualified, {{"apply_deleted_mask", "0"}});
}

//! Active parts of a MergeTree-family table that do not store `column` (written before it was added, and not merged
//! or mutated since)
static uint64_t CountPartsWithout(ClickhouseConnection &connection, const string &database,
                                  const ClickhouseTableEntry &table, const string &column) {
	return QueryCount(connection, "SELECT countIf(NOT has(columns, " + ClickhouseUtils::QuoteLiteral(column) +
	                                  ")) FROM (SELECT groupArray(column) AS columns FROM system.parts_columns WHERE "
	                                  "database = " +
	                                  ClickhouseUtils::QuoteLiteral(database) + " AND table = " +
	                                  ClickhouseUtils::QuoteLiteral(table.name) + " AND active GROUP BY name)");
}

//! Type and NOT NULL changes rewrite the column: ClickHouse refuses them on key columns (ALTER_OF_COLUMN_IS_FORBIDDEN),
//! and an ALIAS column holds no data to convert
static void ThrowIfNotRewritable(const ClickhouseServerColumn &column, const ClickhouseTableEntry &table) {
	if (column.in_key) {
		throw NotImplementedException("Column \"%s\" is part of the sorting, primary, partition or sampling key of "
		                              "ClickHouse table \"%s\", whose type ClickHouse cannot change",
		                              column.info.name, table.name);
	}
	if (column.default_kind == "ALIAS") {
		throw NotImplementedException("Column \"%s\" of ClickHouse table \"%s\" is an ALIAS; change it with "
		                              "clickhouse_execute() instead",
		                              column.info.name, table.name);
	}
}

//! Types ClickHouse cannot wrap in Nullable
static bool CanBeNullable(const ClickhouseTypeNode &node) {
	static const unordered_set<string> NOT_NULLABLE = {"Array",
	                                                   "Tuple",
	                                                   "Map",
	                                                   "Nested",
	                                                   "JSON",
	                                                   "Object",
	                                                   "Variant",
	                                                   "Dynamic",
	                                                   "Point",
	                                                   "Ring",
	                                                   "LineString",
	                                                   "MultiLineString",
	                                                   "Polygon",
	                                                   "MultiPolygon",
	                                                   "Geometry",
	                                                   "AggregateFunction",
	                                                   "SimpleAggregateFunction",
	                                                   "Nothing"};
	return NOT_NULLABLE.find(node.name) == NOT_NULLABLE.end();
}

static string ModifyColumn(const string &qualified, const ClickhouseColumnInfo &column, const string &rest) {
	return "ALTER TABLE " + qualified + " MODIFY COLUMN " + ClickhouseUtils::QuoteIdentifier(column.name) + " " + rest;
}

//! ClickHouse computes a column that a part does not store (one added after the part was written) from the column's
//! current default, so changing the default would change those rows, where DuckDB's SET/DROP DEFAULT never changes a
//! stored value. Stores the column in every part first, with the value it reads as now
static void StoreInEveryPart(ClientContext &context, ClickhouseCatalog &catalog, const string &database,
                             const ClickhouseTableEntry &table, const ClickhouseServerColumn &column,
                             uint64_t missing_parts) {
	auto &name = column.info.name;
	auto sync = ClickhouseDml::MutationsSyncSetting(context);
	// the default would change while the mutation still waits to run: anywhere with 0, and on the other replicas of a
	// Replicated (or ClickHouse Cloud's Shared) table with 1, which waits for this replica only (the default change is
	// not ordered after the mutation there)
	auto &engine = table.GetEngine();
	auto replicated = StringUtil::StartsWith(engine, "Replicated") || StringUtil::StartsWith(engine, "Shared");
	if (sync.second == "0" || (replicated && sync.second != "2")) {
		throw NotImplementedException(
		    "Cannot change the default of column \"%s\" of ClickHouse table \"%s\": %d part(s) do not store the column "
		    "yet, and ClickHouse computes it there from the current default, so it has to be materialized first, which "
		    "ch_mutations_sync = %s would leave running in the background%s; SET ch_mutations_sync = 2, or use "
		    "clickhouse_execute() instead",
		    name, table.name, missing_parts, sync.second, sync.second == "0" ? "" : " on the other replicas");
	}
	auto qualified = ClickhouseUtils::QualifiedName(database, table.name);
	auto quoted = ClickhouseUtils::QuoteIdentifier(name);
	// MATERIALIZE COLUMN refuses a column without a default (BAD_ARGUMENTS); rewriting it with itself stores the value
	// it reads as
	auto sql = column.default_kind.empty()
	               ? "ALTER TABLE " + qualified + " UPDATE " + quoted + " = " + quoted + " WHERE 1"
	               : "ALTER TABLE " + qualified + " MATERIALIZE COLUMN " + quoted;
	ClickhouseDdl::Execute(context, catalog, sql, {sync});
	auto connection = catalog.GetConnectionPool().GetConnection();
	missing_parts = CountPartsWithout(connection.GetConnection(), database, table, name);
	if (missing_parts > 0) {
		throw InvalidInputException("Cannot change the default of column \"%s\" of ClickHouse table \"%s\": %d part(s) "
		                            "still do not store the column after `%s`; the default was not changed",
		                            name, table.name, missing_parts, sql);
	}
}

static ClickhouseAlterStatement SetDefault(ClientContext &context, ClickhouseCatalog &catalog, const string &database,
                                           const ClickhouseTableEntry &table, SetDefaultInfo &info) {
	auto &cached = RequireColumn(table, info.column_name);
	auto &engine = table.GetEngine();
	auto merge_tree = StringUtil::EndsWith(engine, "MergeTree");
	// Memory stores an added column for the rows it already holds
	if (!merge_tree && engine != "Memory") {
		throw NotImplementedException("Cannot change the default of column \"%s\" of ClickHouse table \"%s\" (engine "
		                              "%s): only MergeTree-family and Memory tables are supported; use "
		                              "clickhouse_execute() instead",
		                              cached.name, table.name, engine);
	}
	ClickhouseServerColumn column;
	uint64_t missing_parts = 0;
	{
		auto connection = catalog.GetConnectionPool().GetConnection();
		column = LoadServerColumn(connection.GetConnection(), database, table, cached.name);
		if (merge_tree) {
			missing_parts = CountPartsWithout(connection.GetConnection(), database, table, column.info.name);
		}
	}
	if (column.default_kind == "MATERIALIZED" || column.default_kind == "ALIAS") {
		// MODIFY COLUMN … DEFAULT would silently turn it into a DEFAULT column
		throw NotImplementedException("Column \"%s\" of ClickHouse table \"%s\" is %s, not a column with a DEFAULT; "
		                              "change it with clickhouse_execute() instead",
		                              column.info.name, table.name, column.default_kind);
	}
	auto qualified = ClickhouseUtils::QualifiedName(database, table.name);
	string sql;
	if (!info.expression) {
		// DROP DEFAULT: ClickHouse refuses REMOVE DEFAULT for a column without one
		if (column.default_kind.empty()) {
			return {};
		}
		sql = ModifyColumn(qualified, column.info, "REMOVE DEFAULT");
	} else {
		// ClickHouse keeps the column's type: only the default is replaced
		ColumnDefinition definition(column.info.name, column.info.type);
		definition.SetDefaultValue(info.expression->Copy());
		sql = ModifyColumn(qualified, column.info, ClickhouseDdl::DefaultValueSql(context, definition).substr(1));
	}
	if (missing_parts > 0) {
		StoreInEveryPart(context, catalog, database, table, column, missing_parts);
	}
	return {sql};
}

static ClickhouseAlterStatement ChangeNullability(ClickhouseCatalog &catalog, const string &database,
                                                  const ClickhouseTableEntry &table, const string &name,
                                                  bool nullable) {
	auto &cached = RequireColumn(table, name);
	auto connection = catalog.GetConnectionPool().GetConnection();
	auto column = LoadServerColumn(connection.GetConnection(), database, table, cached.name);
	auto &node = column.info.type_node;
	if (ClickhouseTypes::IsNullable(node) == nullable) {
		return {};
	}
	ThrowIfNotRewritable(column, table);
	auto qualified = ClickhouseUtils::QualifiedName(database, table.name);
	auto wrappers = ClickhouseTypeWrappers::Of(node);
	if (nullable) {
		if (!CanBeNullable(wrappers.base)) {
			throw NotImplementedException("Column \"%s\" (%s) cannot be Nullable in ClickHouse", column.info.name,
			                              column.info.clickhouse_type);
		}
	} else {
		if (!wrappers.nullable) {
			// Nullable inside another wrapper, e.g. SimpleAggregateFunction(any, Nullable(Int32))
			throw NotImplementedException("Column \"%s\" (%s) cannot be made NOT NULL through ALTER TABLE; change it "
			                              "with clickhouse_execute() instead",
			                              column.info.name, column.info.clickhouse_type);
		}
		// converting a NULL to a non-Nullable type fails the mutation, which then blocks the table
		auto nulls = CountStoredRows(connection.GetConnection(), qualified,
		                             ClickhouseUtils::QuoteIdentifier(column.info.name) + " IS NULL");
		if (nulls > 0) {
			throw ConstraintException(
			    "NOT NULL constraint failed: %s.%s (%d stored row(s) of the ClickHouse table hold "
			    "NULL, counting deleted rows not purged yet: ALTER TABLE … APPLY DELETED MASK "
			    "purges those); nothing was changed",
			    table.name, column.info.name, nulls);
		}
	}
	wrappers.nullable = nullable;
	return {ModifyColumn(qualified, column.info, wrappers.Wrap(wrappers.base.text)), true};
}

//! The USING-less form: DuckDB's parser makes it CAST(<column> AS <target type>)
static bool IsPlainTypeChange(const ChangeColumnTypeInfo &info) {
	if (!info.expression || info.expression->GetExpressionType() != ExpressionType::OPERATOR_CAST) {
		return false;
	}
	auto &cast = info.expression->Cast<CastExpression>();
	if (cast.try_cast || cast.child->GetExpressionType() != ExpressionType::COLUMN_REF) {
		return false;
	}
	auto &column = cast.child->Cast<ColumnRefExpression>();
	return !column.IsQualified() && StringUtil::CIEquals(column.GetColumnName(), info.column_name);
}

static bool IsNested(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
	case LogicalTypeId::UNION:
		return true;
	default:
		return false;
	}
}

//! Bits and signedness of an integer type; false for any other type
static bool GetIntegerRange(const LogicalType &type, idx_t &bits, bool &is_signed) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::UTINYINT:
		bits = 8;
		break;
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::USMALLINT:
		bits = 16;
		break;
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::UINTEGER:
		bits = 32;
		break;
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UBIGINT:
		bits = 64;
		break;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		bits = 128;
		break;
	default:
		return false;
	}
	is_signed = type.IsSigned();
	return true;
}

//! Decimal digits of the largest magnitude an integer type holds: 255 or -128, ..., 18446744073709551615
static idx_t IntegerDigits(idx_t bits, bool is_signed) {
	switch (bits) {
	case 8:
		return 3;
	case 16:
		return 5;
	case 32:
		return 10;
	case 64:
		return is_signed ? 19 : 20;
	default:
		return 39;
	}
}

//! Whether ClickHouse's MODIFY COLUMN turns every value of `source` into what DuckDB's ALTER makes of it: the same
//! value, or for VARCHAR the same text. A closed list, each pair checked on ClickHouse 25.8 at the bounds of its
//! source type and with NULL. Everything else -- narrowing, rounding, parsing strings, time zones -- is refused
//! whatever the stored values, since ClickHouse converts out-of-range values without an error (3000000000 to Int32,
//! -1 to UInt32, 12345.67 into Decimal(4, 2), 1900-01-01 to DateTime)
static bool KeepsEveryValue(const LogicalType &source, const LogicalType &target) {
	idx_t source_bits = 0;
	bool source_signed = false;
	auto integer_source = GetIntegerRange(source, source_bits, source_signed);
	switch (target.id()) {
	case LogicalTypeId::VARCHAR:
		return !target.IsJSONType() &&
		       (integer_source || source.id() == LogicalTypeId::DATE || source.id() == LogicalTypeId::ENUM);
	case LogicalTypeId::FLOAT:
		// every integer below 2^24 is a float
		return integer_source && source_bits <= 16;
	case LogicalTypeId::DOUBLE:
		// every integer below 2^53 is a double
		return source.id() == LogicalTypeId::FLOAT || (integer_source && source_bits <= 32);
	case LogicalTypeId::DECIMAL: {
		auto integer_digits = DecimalType::GetWidth(target) - DecimalType::GetScale(target);
		if (integer_source) {
			return integer_digits >= IntegerDigits(source_bits, source_signed);
		}
		return source.id() == LogicalTypeId::DECIMAL &&
		       DecimalType::GetScale(target) >= DecimalType::GetScale(source) &&
		       integer_digits >= DecimalType::GetWidth(source) - DecimalType::GetScale(source);
	}
	case LogicalTypeId::TIMESTAMP:
		// midnight; DateTime64(6) holds all of Date32's range. Not TIMESTAMPTZ: DuckDB takes midnight in the session's
		// time zone
		return source.id() == LogicalTypeId::DATE;
	default: {
		idx_t target_bits = 0;
		bool target_signed = false;
		return integer_source && GetIntegerRange(target, target_bits, target_signed) && target_bits > source_bits &&
		       (target_signed || !source_signed);
	}
	}
}

static ClickhouseAlterStatement ChangeType(ClickhouseCatalog &catalog, const string &database,
                                           const ClickhouseTableEntry &table, ChangeColumnTypeInfo &info) {
	auto &cached = RequireColumn(table, info.column_name);
	if (!IsPlainTypeChange(info)) {
		throw NotImplementedException("ALTER COLUMN … TYPE … USING is not supported for ClickHouse tables; convert "
		                              "the column with clickhouse_execute() instead");
	}
	ClickhouseServerColumn column;
	{
		auto connection = catalog.GetConnectionPool().GetConnection();
		column = LoadServerColumn(connection.GetConnection(), database, table, cached.name);
	}
	auto &source = column.info.type;
	auto &target = info.target_type;
	if (source == target) {
		return {};
	}
	auto &name = column.info.name;
	if (IsNested(source) || IsNested(target)) {
		// an element that does not convert becomes NULL (['1', 'x'] to [1, NULL])
		throw NotImplementedException("Cannot change the type of column \"%s\" of ClickHouse table \"%s\": only scalar "
		                              "columns can change type through ALTER TABLE; use clickhouse_execute() instead",
		                              name, table.name);
	}
	if (!KeepsEveryValue(source, target)) {
		throw NotImplementedException(
		    "Cannot change the type of column \"%s\" of ClickHouse table \"%s\" from %s to %s: only conversions that "
		    "keep every value exactly are supported (to a wider integer, DECIMAL or floating-point type that holds "
		    "every value, FLOAT to DOUBLE, integers, DATE and ENUM to VARCHAR, DATE to TIMESTAMP); convert the column "
		    "with clickhouse_execute() instead",
		    name, table.name, source.ToString(), target.ToString());
	}
	// the DuckDB-level conversion is only what ClickHouse does if the column holds exactly the type DuckDB reads it
	// as: not, e.g., BFloat16 (FLOAT). An enum converts by label whatever its values
	auto wrappers = ClickhouseTypeWrappers::Of(column.info.type_node);
	auto &base = wrappers.base;
	auto canonical = source.id() == LogicalTypeId::ENUM ? string() : ClickhouseDdlTypes::ToClickhouse(source, false);
	auto stored_exactly = base.text == canonical || (source.id() == LogicalTypeId::DATE && base.name == "Date") ||
	                      (source.id() == LogicalTypeId::ENUM && (base.name == "Enum8" || base.name == "Enum16"));
	if (!stored_exactly) {
		throw NotImplementedException("Cannot change the type of column \"%s\" of ClickHouse table \"%s\": its "
		                              "ClickHouse type %s is not one DuckDB converts exactly; use clickhouse_execute() "
		                              "instead",
		                              name, table.name, column.info.clickhouse_type);
	}
	ThrowIfNotRewritable(column, table);
	auto qualified = ClickhouseUtils::QualifiedName(database, table.name);
	return {ModifyColumn(qualified, column.info, wrappers.Wrap(ClickhouseDdlTypes::ToClickhouse(target, false))), true};
}

ClickhouseAlterStatement ClickhouseDdl::AlterTable(ClientContext &context, ClickhouseCatalog &catalog,
                                                   const string &database, const ClickhouseTableEntry &table,
                                                   AlterTableInfo &info) {
	auto qualified = ClickhouseUtils::QualifiedName(database, table.name);
	switch (info.alter_table_type) {
	case AlterTableType::ADD_COLUMN: {
		auto &add = info.Cast<AddColumnInfo>();
		auto &new_name = add.new_column.Name();
		// case-insensitively: ClickHouse would accept "ID" next to "id", and the next metadata load could not list both
		for (auto &column : table.GetClickhouseColumns()) {
			if (StringUtil::CIEquals(column.name, new_name)) {
				if (add.if_column_not_exists) {
					return {};
				}
				throw CatalogException("Column with name \"%s\" already exists in ClickHouse table \"%s\"%s", new_name,
				                       table.name, SpelledAs(column, new_name));
			}
		}
		// scalar columns are added Nullable (Array/Tuple/Map/JSON never are): DuckDB's ADD COLUMN carries no NOT
		// NULL constraint
		auto column_sql = ColumnSql(context, add.new_column, true);
		auto &engine = table.GetEngine();
		if (add.new_column.HasDefaultValue() && !StringUtil::EndsWith(engine, "MergeTree")) {
			// only the MergeTree family computes the default for the rows a table already holds: Memory (and the
			// Buffer and Merge proxies) gives them NULL or the type's zero, and cannot store the column afterwards
			// (a mutation finds it in no block)
			auto connection = catalog.GetConnectionPool().GetConnection();
			if (QueryCount(connection.GetConnection(), "SELECT count() FROM " + qualified) > 0) {
				throw NotImplementedException(
				    "Cannot add column \"%s\" with a DEFAULT to ClickHouse table \"%s\" (engine %s): it holds rows, "
				    "which ClickHouse would give NULL rather than the default (only MergeTree-family tables compute it "
				    "for existing rows); add the column without a DEFAULT, or use clickhouse_execute() instead",
				    new_name, table.name, engine);
			}
		}
		// IF NOT EXISTS still sent: the cached column list may be stale
		return {"ALTER TABLE " + qualified + " ADD COLUMN " + (add.if_column_not_exists ? "IF NOT EXISTS " : "") +
		        column_sql};
	}
	case AlterTableType::REMOVE_COLUMN: {
		auto &remove = info.Cast<RemoveColumnInfo>();
		auto column = ResolveColumn(table, remove.removed_column);
		if (!column) {
			if (remove.if_column_exists) {
				return {};
			}
			throw CatalogException("Column with name \"%s\" does not exist in ClickHouse table \"%s\"",
			                       remove.removed_column, table.name);
		}
		// IF EXISTS still sent: the cached column list may be stale
		return {"ALTER TABLE " + qualified + " DROP COLUMN " + (remove.if_column_exists ? "IF EXISTS " : "") +
		        ClickhouseUtils::QuoteIdentifier(column->name)};
	}
	case AlterTableType::RENAME_COLUMN: {
		auto &rename = info.Cast<RenameColumnInfo>();
		auto column = ResolveColumn(table, rename.old_name);
		if (!column) {
			throw CatalogException("Column with name \"%s\" does not exist in ClickHouse table \"%s\"", rename.old_name,
			                       table.name);
		}
		// renaming a column to another spelling of its own name (id -> ID) is fine; to any other column's is not
		for (auto &other : table.GetClickhouseColumns()) {
			if (&other != column.get() && StringUtil::CIEquals(other.name, rename.new_name)) {
				throw CatalogException("Column with name \"%s\" already exists in ClickHouse table \"%s\"%s",
				                       rename.new_name, table.name, SpelledAs(other, rename.new_name));
			}
		}
		return {"ALTER TABLE " + qualified + " RENAME COLUMN " + ClickhouseUtils::QuoteIdentifier(column->name) +
		        " TO " + ClickhouseUtils::QuoteIdentifier(rename.new_name)};
	}
	case AlterTableType::RENAME_TABLE: {
		auto &rename = info.Cast<RenameTableInfo>();
		return {"RENAME TABLE " + qualified + " TO " + ClickhouseUtils::QualifiedName(database, rename.new_table_name)};
	}
	case AlterTableType::SET_DEFAULT:
		return SetDefault(context, catalog, database, table, info.Cast<SetDefaultInfo>());
	case AlterTableType::DROP_NOT_NULL:
		return ChangeNullability(catalog, database, table, info.Cast<DropNotNullInfo>().column_name, true);
	case AlterTableType::SET_NOT_NULL:
		return ChangeNullability(catalog, database, table, info.Cast<SetNotNullInfo>().column_name, false);
	case AlterTableType::ALTER_COLUMN_TYPE:
		return ChangeType(catalog, database, table, info.Cast<ChangeColumnTypeInfo>());
	default:
		throw NotImplementedException("This ALTER TABLE operation is not supported for ClickHouse tables (only ADD "
		                              "COLUMN, DROP COLUMN, RENAME COLUMN, RENAME TO and ALTER COLUMN SET/DROP "
		                              "DEFAULT, SET/DROP NOT NULL and TYPE are); run it with clickhouse_execute() "
		                              "instead");
	}
}

} // namespace duckdb
