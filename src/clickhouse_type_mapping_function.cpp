#include "clickhouse_type_mapping_function.hpp"

#include "clickhouse_types.hpp"

namespace duckdb {

struct ClickhouseTypeMappingData : public TableFunctionData {
	string duckdb_type;
	string read_expression;
	bool nullable = false;
	bool pushdown = false;
	bool readable = true;
	bool finished = false;
};

static unique_ptr<FunctionData> TypeMappingBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("clickhouse_type_mapping: the type cannot be NULL");
	}
	auto node = ClickhouseTypeParser::Parse(StringValue::Get(input.inputs[0]));
	auto mapped = ClickhouseTypes::ToDuckDB(node);
	auto result = make_uniq<ClickhouseTypeMappingData>();
	result->duckdb_type = mapped.type.ToString();
	result->read_expression = ClickhouseTypes::ReadExpression(node, "c");
	result->nullable = ClickhouseTypes::IsNullable(node);
	result->pushdown = ClickhouseTypes::SupportsPushdown(node);
	result->readable = mapped.readable;

	names = {"duckdb_type", "read_expression", "nullable", "pushdown", "readable"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BOOLEAN,
	                LogicalType::BOOLEAN};
	return std::move(result);
}

static void TypeMappingFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<ClickhouseTypeMappingData>();
	if (data.finished) {
		return;
	}
	output.SetValue(0, 0, Value(data.duckdb_type));
	output.SetValue(1, 0, Value(data.read_expression));
	output.SetValue(2, 0, Value::BOOLEAN(data.nullable));
	output.SetValue(3, 0, Value::BOOLEAN(data.pushdown));
	output.SetValue(4, 0, Value::BOOLEAN(data.readable));
	output.SetCardinality(1);
	data.finished = true;
}

ClickhouseTypeMappingFunction::ClickhouseTypeMappingFunction()
    : TableFunction("clickhouse_type_mapping", {LogicalType::VARCHAR}, TypeMappingFunction, TypeMappingBind) {
}

} // namespace duckdb
