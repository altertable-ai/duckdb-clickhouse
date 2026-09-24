#include "clickhouse_execute.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "storage/clickhouse_catalog.hpp"

namespace duckdb {

struct ClickhouseExecuteBindData : public TableFunctionData {
	//! Resolved again when the statement runs: a prepared statement can outlive the ATTACH it was bound against
	string database_name;
	string sql;

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<ClickhouseExecuteBindData>();
		result->database_name = database_name;
		result->sql = sql;
		return std::move(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ClickhouseExecuteBindData>();
		return database_name == other.database_name && sql == other.sql;
	}
};

struct ClickhouseExecuteGlobalState : public GlobalTableFunctionState {
	bool finished = false;
};

//! The writable ClickHouse catalog attached as `database_name`
static ClickhouseCatalog &GetWritableCatalog(ClientContext &context, const string &database_name) {
	auto &catalog = ClickhouseCatalog::GetAttachedDatabase(context, database_name, "clickhouse_execute");
	catalog.ThrowIfReadOnly();
	return catalog;
}

static unique_ptr<FunctionData> ClickhouseExecuteBind(ClientContext &context, TableFunctionBindInput &input,
                                                      vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("Parameters to clickhouse_execute cannot be NULL");
	}
	auto result = make_uniq<ClickhouseExecuteBindData>();
	result->database_name = StringValue::Get(input.inputs[0]);
	result->sql = ClickhouseUtils::StripTrailingSemicolons(StringValue::Get(input.inputs[1]));
	if (result->sql.empty()) {
		throw BinderException("clickhouse_execute: the statement cannot be empty");
	}
	// fail early, at bind time, for a missing, non-ClickHouse or read-only database
	GetWritableCatalog(context, result->database_name);
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ClickhouseExecuteInitGlobal(ClientContext &context,
                                                                        TableFunctionInitInput &input) {
	return make_uniq<ClickhouseExecuteGlobalState>();
}

static void ClickhouseExecuteExecute(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<ClickhouseExecuteGlobalState>();
	if (state.finished) {
		return;
	}
	state.finished = true;
	auto &bind_data = data.bind_data->Cast<ClickhouseExecuteBindData>();
	auto &catalog = GetWritableCatalog(context, bind_data.database_name);
	bool returned_no_rows;
	try {
		auto connection = catalog.StartWrite(context);
		returned_no_rows = connection->Execute(bind_data.sql);
	} catch (...) {
		// a failed statement can still have changed something (e.g. a multi-part ALTER)
		catalog.ClearCache();
		throw;
	}
	catalog.ClearCache();
	if (!returned_no_rows) {
		throw InvalidInputException("clickhouse_execute: the statement returned rows, which clickhouse_execute cannot "
		                            "return (the statement was still run); use clickhouse_query() to read query "
		                            "results");
	}
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(true));
}

ClickhouseExecuteFunction::ClickhouseExecuteFunction()
    : TableFunction("clickhouse_execute", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ClickhouseExecuteExecute,
                    ClickhouseExecuteBind, ClickhouseExecuteInitGlobal) {
}

} // namespace duckdb
