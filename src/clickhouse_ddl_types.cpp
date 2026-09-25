#include "clickhouse_ddl_types.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

//! Named EnumTypeSql (not EnumType) because that name would otherwise shadow DuckDB's own EnumType struct, which
//! this function calls into (EnumType::GetSize / EnumType::GetValuesInsertOrder)
static string EnumTypeSql(const LogicalType &type) {
	auto size = EnumType::GetSize(type);
	if (size > 32767) {
		throw NotImplementedException("DuckDB ENUM with %d labels has no ClickHouse equivalent (Enum16 holds at most "
		                              "32767); create the table with clickhouse_execute() instead",
		                              static_cast<uint64_t>(size));
	}
	auto &labels = EnumType::GetValuesInsertOrder(type);
	auto label_data = FlatVector::GetData<string_t>(labels);
	vector<string> entries;
	for (idx_t i = 0; i < size; i++) {
		auto label = label_data[i].GetString();
		// ClickHouse itself accepts these (escaped), but clickhouse-cpp 2.6.2's type parser cannot parse an escaped
		// quote inside an Enum's type string, so every later SELECT or INSERT of the column would fail: refuse to
		// create a table DuckDB could not use
		if (label.find('\'') != string::npos || label.find('\\') != string::npos) {
			throw NotImplementedException("ENUM label %s contains a quote or a backslash, which clickhouse-cpp cannot "
			                              "read or write in ClickHouse Enum columns; use labels without them, or a "
			                              "VARCHAR column",
			                              ClickhouseUtils::QuoteLiteral(label));
		}
		entries.push_back(ClickhouseUtils::QuoteLiteral(label) + " = " + to_string(i + 1));
	}
	return (size <= 127 ? "Enum8(" : "Enum16(") + StringUtil::Join(entries, ", ") + ")";
}

string ClickhouseDdlTypes::ToClickhouse(const LogicalType &type, bool nullable) {
	string result;
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		result = "Bool";
		break;
	case LogicalTypeId::TINYINT:
		result = "Int8";
		break;
	case LogicalTypeId::SMALLINT:
		result = "Int16";
		break;
	case LogicalTypeId::INTEGER:
		result = "Int32";
		break;
	case LogicalTypeId::BIGINT:
		result = "Int64";
		break;
	case LogicalTypeId::UTINYINT:
		result = "UInt8";
		break;
	case LogicalTypeId::USMALLINT:
		result = "UInt16";
		break;
	case LogicalTypeId::UINTEGER:
		result = "UInt32";
		break;
	case LogicalTypeId::UBIGINT:
		result = "UInt64";
		break;
	case LogicalTypeId::HUGEINT:
		result = "Int128";
		break;
	case LogicalTypeId::UHUGEINT:
		result = "UInt128";
		break;
	case LogicalTypeId::FLOAT:
		result = "Float32";
		break;
	case LogicalTypeId::DOUBLE:
		result = "Float64";
		break;
	case LogicalTypeId::DECIMAL:
		result = StringUtil::Format("Decimal(%d, %d)", static_cast<int32_t>(DecimalType::GetWidth(type)),
		                            static_cast<int32_t>(DecimalType::GetScale(type)));
		break;
	case LogicalTypeId::VARCHAR:
		if (type.IsJSONType()) {
			return "JSON";
		}
		result = "String";
		break;
	case LogicalTypeId::BLOB:
		result = "String";
		break;
	case LogicalTypeId::DATE:
		result = "Date32";
		break;
	case LogicalTypeId::TIMESTAMP_SEC:
		result = "DateTime('UTC')";
		break;
	case LogicalTypeId::TIMESTAMP_MS:
		result = "DateTime64(3, 'UTC')";
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		result = "DateTime64(6, 'UTC')";
		break;
	case LogicalTypeId::TIMESTAMP_NS:
		result = "DateTime64(9, 'UTC')";
		break;
	case LogicalTypeId::TIME:
		result = "Time64(6)";
		break;
	case LogicalTypeId::TIME_NS:
		result = "Time64(9)";
		break;
	case LogicalTypeId::UUID:
		result = "UUID";
		break;
	case LogicalTypeId::ENUM:
		result = EnumTypeSql(type);
		break;
	case LogicalTypeId::LIST:
		return "Array(" + ToClickhouse(ListType::GetChildType(type), true) + ")";
	case LogicalTypeId::STRUCT: {
		vector<string> fields;
		for (auto &child : StructType::GetChildTypes(type)) {
			// an unnamed STRUCT (e.g. from row(1, 'a')): ClickHouse rejects an empty Tuple element name, and an
			// unnamed Tuple would read back as a STRUCT with fields "1", "2", … -- a different type
			if (child.first.empty()) {
				throw NotImplementedException("STRUCT fields must be named for ClickHouse Tuple columns (got %s); name "
				                              "them, e.g. {'a': 1, 'b': 'x'} instead of row(1, 'x')",
				                              type.ToString());
			}
			fields.push_back(ClickhouseUtils::QuoteIdentifier(child.first) + " " + ToClickhouse(child.second, true));
		}
		return "Tuple(" + StringUtil::Join(fields, ", ") + ")";
	}
	case LogicalTypeId::MAP:
		return "Map(" + ToClickhouse(MapType::KeyType(type), false) + ", " + ToClickhouse(MapType::ValueType(type), true) +
		       ")";
	default:
		throw NotImplementedException("DuckDB type %s has no ClickHouse equivalent; create the table with "
		                              "clickhouse_execute() instead",
		                              type.ToString());
	}
	return nullable ? "Nullable(" + result + ")" : result;
}

} // namespace duckdb
