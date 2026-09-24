#include "storage/clickhouse_insert.hpp"

#include "clickhouse_utils.hpp"
#include "clickhouse_writer.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_connection_pool.hpp"
#include "storage/clickhouse_table_entry.hpp"
#include "storage/clickhouse_transaction.hpp"

namespace duckdb {

ClickhouseInsert::ClickhouseInsert(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseTableEntry &table_p,
                                   vector<ClickhouseInsertColumn> columns_p)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, op.types, 1), table(table_p),
      columns(std::move(columns_p)) {
	insert_sql = BuildInsertQuery(table, columns);
}

vector<ClickhouseInsertColumn>
ClickhouseInsert::GetInsertColumns(ClickhouseTableEntry &table,
                                   const physical_index_vector_t<idx_t> &column_index_map) {
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
			break;
		case ClickhouseWriteMode::SERVER_CONVERSION:
			// Task 4 replaces this case
			throw NotImplementedException("Inserting into ClickHouse column \"%s\" of type %s is not supported yet; "
			                              "use clickhouse_execute() instead",
			                              column.name, column.clickhouse_type);
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
	for (auto &column : columns) {
		names.push_back(ClickhouseUtils::QuoteIdentifier(column.column.name));
	}
	return "INSERT INTO " + ClickhouseUtils::QuoteIdentifier(table.schema.name) + "." +
	       ClickhouseUtils::QuoteIdentifier(table.name) + " (" + StringUtil::Join(names, ", ") + ") VALUES";
}

//===--------------------------------------------------------------------===//
// State
//===--------------------------------------------------------------------===//
class ClickhouseInsertGlobalState : public GlobalSinkState {
public:
	~ClickhouseInsertGlobalState() override {
		// Finalize() did not run -- an error here, in a conversion, or upstream: abandon the INSERT so the connection
		// is closed instead of being reused mid-insert. ClickHouse may already have committed the blocks sent so far
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

	ClickhousePoolConnection connection;
	//! What BeginInsert() returned: the names and types of the values to send, in order
	clickhouse::Block header;
	//! The rows of the next block, one column per header column
	vector<clickhouse::ColumnRef> pending;
	idx_t pending_rows = 0;
	idx_t block_size = 65536;
	idx_t insert_count = 0;
};

unique_ptr<GlobalSinkState> ClickhouseInsert::GetGlobalSinkState(ClientContext &context) const {
	auto &catalog = table.catalog.Cast<ClickhouseCatalog>();
	ClickhouseTransaction::Get(context, catalog).MarkWritten();
	auto result = make_uniq<ClickhouseInsertGlobalState>();
	Value block_size;
	if (context.TryGetCurrentSetting("ch_insert_block_size", block_size) && !block_size.IsNull()) {
		result->block_size = UBigIntValue::Get(block_size);
	}
	result->connection = catalog.GetConnectionPool().GetConnection();
	result->header = result->connection->BeginInsert(insert_sql);
	if (result->header.GetColumnCount() != columns.size()) {
		throw InternalException("ClickHouse INSERT header has %d columns, expected %d",
		                        static_cast<uint64_t>(result->header.GetColumnCount()),
		                        static_cast<uint64_t>(columns.size()));
	}
	for (idx_t i = 0; i < columns.size(); i++) {
		result->pending.push_back(result->header[i]->CloneEmpty());
	}
	return std::move(result);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
SinkResultType ClickhouseInsert::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &gstate = input.global_state.Cast<ClickhouseInsertGlobalState>();
	for (idx_t i = 0; i < columns.size(); i++) {
		ClickhouseWriter::AppendVector(chunk.data[columns[i].source_index], chunk.size(), gstate.pending[i],
		                               columns[i].column.name);
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
	return "CLICKHOUSE_INSERT";
}

InsertionOrderPreservingMap<string> ClickhouseInsert::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Table"] = table.schema.name + "." + table.name;
	return result;
}

} // namespace duckdb
