#include "storage/clickhouse_insert.hpp"

#include "clickhouse_utils.hpp"
#include "clickhouse_writer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_connection_pool.hpp"
#include "storage/clickhouse_ddl.hpp"
#include "storage/clickhouse_table_entry.hpp"

namespace duckdb {

ClickhouseInsert::ClickhouseInsert(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseTableEntry &table_p,
                                   vector<ClickhouseInsertColumn> columns_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1),
      catalog_name(table_p.catalog.GetName()), database_name(table_p.schema.name), table_name(table_p.name),
      columns(std::move(columns_p)) {
	insert_sql = BuildInsertQuery(table_p, columns);
}

ClickhouseInsert::ClickhouseInsert(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseCatalog &catalog,
                                   const string &database, unique_ptr<CreateTableInfo> create_info_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1), catalog_name(catalog.GetName()),
      database_name(database), table_name(create_info_p->table), create_info(std::move(create_info_p)) {
}

vector<ClickhouseInsertColumn>
ClickhouseInsert::GetInsertColumns(ClickhouseTableEntry &table,
                                   const physical_index_vector_t<idx_t> &column_index_map) {
	// column_index_map is indexed by DuckDB column, which lines up with clickhouse_columns only without a collision
	table.ThrowIfColumnsCollide();
	auto &clickhouse_columns = table.GetClickhouseColumns();
	vector<ClickhouseInsertColumn> result;
	for (idx_t i = 0; i < clickhouse_columns.size(); i++) {
		auto source_index = i;
		if (!column_index_map.empty()) {
			source_index = column_index_map[PhysicalIndex(i)];
			if (source_index == DConstants::INVALID_INDEX) {
				// not listed: ClickHouse fills in its DEFAULT
				continue;
			}
		}
		auto &column = clickhouse_columns[i];
		if (column.default_kind == "MATERIALIZED" || column.default_kind == "ALIAS") {
			throw BinderException("Column \"%s\" of ClickHouse table \"%s\" is a %s column, which ClickHouse computes "
			                      "itself; list the columns to insert explicitly, e.g. INSERT INTO %s (…) VALUES …",
			                      column.name, table.name, column.default_kind, table.name);
		}
		switch (ClickhouseWriter::GetWriteMode(column.type_node)) {
		case ClickhouseWriteMode::NATIVE:
		case ClickhouseWriteMode::SERVER_CONVERSION:
			break;
		case ClickhouseWriteMode::UNSUPPORTED:
			throw NotImplementedException("Cannot insert into ClickHouse column \"%s\" of type %s; insert into it "
			                              "with clickhouse_execute() instead",
			                              column.name, column.clickhouse_type);
		}
		result.push_back(ClickhouseInsertColumn {column, source_index});
	}
	if (result.empty()) {
		throw NotImplementedException("INSERT into ClickHouse table \"%s\" must write at least one column",
		                              table.name);
	}
	return result;
}

string ClickhouseInsert::BuildInsertQuery(ClickhouseTableEntry &table, const vector<ClickhouseInsertColumn> &columns) {
	vector<string> names;
	vector<string> input_columns;
	vector<string> select_list;
	bool server_conversion = false;
	for (idx_t i = 0; i < columns.size(); i++) {
		auto &column = columns[i].column;
		names.push_back(ClickhouseUtils::QuoteIdentifier(column.name));
		// input() columns get generated names, so column names never need quoting inside the structure literal
		auto input_name = "c" + to_string(i + 1);
		if (ClickhouseWriter::GetWriteMode(column.type_node) == ClickhouseWriteMode::SERVER_CONVERSION) {
			server_conversion = true;
			input_columns.push_back(input_name + " " + ClickhouseWriter::ServerInputType(column.type_node));
			select_list.push_back(ClickhouseWriter::ServerConversion(column.type_node, input_name));
		} else {
			input_columns.push_back(input_name + " " + column.clickhouse_type);
			select_list.push_back(input_name);
		}
	}
	auto target = "INSERT INTO " + ClickhouseUtils::QualifiedName(table.schema.name, table.name) + " (" +
	              StringUtil::Join(names, ", ") + ")";
	if (!server_conversion) {
		return target + " VALUES";
	}
	// values ClickHouse converts are sent as input() columns and converted by the SELECT; the header BeginInsert()
	// returns then describes input()'s structure, which ClickhouseWriter fills like any other INSERT.
	// FORMAT Native must trail the whole SELECT (input() otherwise fails with "Unknown format"); it must not sit
	// between the column list and SELECT, which makes the server read the rest of the query text as literal data
	return target + " SELECT " + StringUtil::Join(select_list, ", ") + " FROM input(" +
	       ClickhouseUtils::QuoteLiteral(StringUtil::Join(input_columns, ", ")) + ") FORMAT Native";
}

//===--------------------------------------------------------------------===//
// State
//===--------------------------------------------------------------------===//
class ClickhouseInsertGlobalState : public GlobalSinkState {
public:
	~ClickhouseInsertGlobalState() override {
		// Finalize() did not run -- an error here, in a conversion, or upstream: abandon the INSERT so the connection
		// is closed instead of being reused mid-insert. ClickHouse may already have committed the blocks sent so far.
		// Without a connection the INSERT was never started (no rows yet, or StartInsert() failed before taking one)
		// and there is nothing to abandon
		if (connection && connection->IsInserting()) {
			connection->AbortInsert();
			connection.Invalidate();
		}
	}

	//! Sends the pending rows as one block
	void Flush() {
		if (pending_rows == 0) {
			return;
		}
		clickhouse::Block block;
		for (idx_t i = 0; i < pending.size(); i++) {
			block.AppendColumn(header.GetColumnName(i), pending[i]);
			pending[i] = header[i]->CloneEmpty();
		}
		pending_rows = 0;
		connection->SendInsertBlock(block);
	}

	//! Taken by the first Sink() (ClickhouseInsert::StartInsert()), returned to the pool by Finalize()
	ClickhousePoolConnection connection;
	//! What BeginInsert() returned: the names and types of the values to send, in order
	clickhouse::Block header;
	//! The rows of the next block, one column per header column
	vector<clickhouse::ColumnRef> pending;
	idx_t pending_rows = 0;
	idx_t block_size = 65536;
	idx_t insert_count = 0;
	//! The target: copied from the operator for INSERT, resolved after creating the table for CTAS
	vector<ClickhouseInsertColumn> columns;
	string insert_sql;
	//! Set once `columns` / `insert_sql` are ready (see PrepareTarget)
	bool target_ready = false;
	//! CTAS with IF NOT EXISTS on an existing table: write nothing
	bool skip = false;
};

//! Makes gstate.columns / insert_sql ready. For CTAS, creates the table first, or finds that IF NOT EXISTS applies
static void PrepareTarget(const ClickhouseInsert &op, ClientContext &context, ClickhouseInsertGlobalState &gstate) {
	if (gstate.target_ready) {
		return;
	}
	gstate.target_ready = true;
	if (!op.create_info) {
		gstate.columns = op.columns;
		gstate.insert_sql = op.insert_sql;
		return;
	}
	auto &catalog = ClickhouseCatalog::GetAttachedDatabase(context, op.catalog_name, "CREATE TABLE AS");
	auto &info = *op.create_info;
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		// a fresh read: another connection (even another alias attached to the same server) may have created the
		// table after this database's schema/table cache was last populated, and the planner routes here whenever
		// the *cached* catalog had no such table, so the cache can be stale in exactly the case that matters
		catalog.ClearCache();
		if (ClickhouseDdl::LookupTable(context, catalog, op.database_name, info.table)) {
			gstate.skip = true;
			return;
		}
	}
	auto &table = ClickhouseDdl::CreateTable(context, catalog, op.database_name, info);
	// IF NOT EXISTS can still race with a concurrent CREATE between the check above and here: the CREATE TABLE
	// statement is then a no-op on the server and CreateTable() returns the pre-existing table, whose columns may
	// not match this query at all. Inserting into it on that assumption could read past the end of the input chunk
	// (the existing table has more columns) or silently drop trailing query columns (fewer columns), so the target
	// is verified before it is ever built
	auto &table_columns = table.GetClickhouseColumns();
	bool mismatch = table_columns.size() != info.columns.LogicalColumnCount();
	if (!mismatch) {
		idx_t i = 0;
		for (auto &column : info.columns.Logical()) {
			if (!StringUtil::CIEquals(table_columns[i].name, column.Name())) {
				mismatch = true;
				break;
			}
			i++;
		}
	}
	if (mismatch) {
		throw InvalidInputException("ClickHouse table \"%s\".\"%s\" already exists with different columns; CREATE "
		                            "TABLE … AS did not write into it",
		                            op.database_name, info.table);
	}
	// every column, in query order: the CTAS input chunk has one column per created column
	gstate.columns = ClickhouseInsert::GetInsertColumns(table, physical_index_vector_t<idx_t>());
	gstate.insert_sql = ClickhouseInsert::BuildInsertQuery(table, gstate.columns);
}

unique_ptr<GlobalSinkState> ClickhouseInsert::GetGlobalSinkState(ClientContext &context) const {
	// nothing is sent to ClickHouse yet: see StartInsert()
	auto result = make_uniq<ClickhouseInsertGlobalState>();
	Value block_size;
	if (context.TryGetCurrentSetting("ch_insert_block_size", block_size) && !block_size.IsNull()) {
		result->block_size = UBigIntValue::Get(block_size);
	}
	return std::move(result);
}

void ClickhouseInsert::StartInsert(ClientContext &context, ClickhouseInsertGlobalState &gstate) const {
	D_ASSERT(!gstate.connection);
	PrepareTarget(*this, context, gstate);
	// belt-and-suspenders against a target that (despite PrepareTarget's own checks) does not match this
	// operator's single child: every gstate.columns[i].source_index must be a valid index into its output chunk,
	// checked here rather than trusted, so a mismatch throws instead of reading past the end of the chunk
	D_ASSERT(!children.empty());
	auto input_column_count = children[0].get().GetTypes().size();
	if (gstate.columns.size() != input_column_count) {
		throw InvalidInputException(
		    "ClickHouse INSERT target for table \"%s\" has %d columns, but the input has %d: run CALL "
		    "clickhouse_clear_cache() and retry",
		    table_name, static_cast<uint64_t>(gstate.columns.size()), static_cast<uint64_t>(input_column_count));
	}
	for (auto &column : gstate.columns) {
		if (column.source_index >= input_column_count) {
			throw InvalidInputException("ClickHouse INSERT target for table \"%s\" references a column the input "
			                            "does not have: run CALL clickhouse_clear_cache() and retry",
			                            table_name);
		}
	}
	// resolved again rather than kept from planning: see catalog_name
	auto &catalog = ClickhouseCatalog::GetAttachedDatabase(context, catalog_name, "INSERT");
	gstate.connection = catalog.StartWrite(context);
	// Time/Time64 columns need this setting (ClickHouse 25.x); servers that do not know it ignore it. Tables
	// DuckDB creates map TIME to Time64 (see ClickhouseDdlTypes), so every INSERT needs it, not just DDL
	gstate.header = gstate.connection->BeginInsert(gstate.insert_sql, {{"enable_time_time64_type", "1"}});
	if (gstate.header.GetColumnCount() != gstate.columns.size()) {
		// not InternalException: this means the cached column list is stale (another connection changed the
		// table since it was cached), and InternalException would invalidate the whole DuckDB instance for
		// every later query in DuckDB v1.5.4
		throw InvalidInputException(
		    "ClickHouse INSERT header for table \"%s\" has %d columns, expected %d: the table changed since its "
		    "metadata was cached; run CALL clickhouse_clear_cache() and retry",
		    table_name, static_cast<uint64_t>(gstate.header.GetColumnCount()),
		    static_cast<uint64_t>(gstate.columns.size()));
	}
	for (idx_t i = 0; i < gstate.columns.size(); i++) {
		gstate.pending.push_back(gstate.header[i]->CloneEmpty());
	}
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType ClickhouseInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<ClickhouseInsertGlobalState>();
	if (chunk.size() == 0) {
		return SinkResultType::NEED_MORE_INPUT;
	}
	if (!gstate.target_ready) {
		PrepareTarget(*this, context.client, gstate);
	}
	if (gstate.skip) {
		return SinkResultType::FINISHED;
	}
	if (!gstate.connection) {
		StartInsert(context.client, gstate);
	}
	for (idx_t i = 0; i < gstate.columns.size(); i++) {
		ClickhouseWriter::AppendVector(chunk.data[gstate.columns[i].source_index], chunk.size(), gstate.pending[i],
		                               gstate.columns[i].column.name);
	}
	gstate.pending_rows += chunk.size();
	gstate.insert_count += chunk.size();
	if (gstate.pending_rows >= gstate.block_size) {
		gstate.Flush();
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkFinalizeType ClickhouseInsert::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                            OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<ClickhouseInsertGlobalState>();
	if (create_info) {
		// an empty CTAS (no rows arrived) still creates the table
		PrepareTarget(*this, context, gstate);
	}
	if (!gstate.connection) {
		// no rows arrived (e.g. an INSERT … SELECT that selected nothing): nothing was started, nothing is sent
		return SinkFinalizeType::READY;
	}
	gstate.Flush();
	gstate.connection->EndInsert();
	// back to the pool right away rather than when the query ends
	gstate.connection = ClickhousePoolConnection();
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
SourceResultType ClickhouseInsert::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                   OperatorSourceInput &input) const {
	auto &gstate = sink_state->Cast<ClickhouseInsertGlobalState>();
	chunk.SetCardinality(1);
	chunk.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(gstate.insert_count)));
	return SourceResultType::FINISHED;
}

string ClickhouseInsert::GetName() const {
	return create_info ? "CLICKHOUSE_CREATE_TABLE_AS" : "CLICKHOUSE_INSERT";
}

InsertionOrderPreservingMap<string> ClickhouseInsert::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table"] = database_name + "." + table_name;
	return result;
}

} // namespace duckdb
