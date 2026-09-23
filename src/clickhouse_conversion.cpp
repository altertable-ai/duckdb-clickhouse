#include "clickhouse_conversion.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <clickhouse/client.h>
#include <clickhouse/columns/bool.h>

namespace duckdb {

namespace ch = clickhouse;

[[noreturn]] static void ThrowUnexpectedColumn(const ch::ColumnRef &column, const Vector &result,
                                                const string &column_name) {
	throw InternalException("ClickHouse column \"%s\" of type %s cannot be converted to DuckDB %s", column_name,
	                         column->Type()->GetName(), result.GetType().ToString());
}

static int64_t PowerOfTen(idx_t exponent) {
	int64_t result = 1;
	for (idx_t i = 0; i < exponent; i++) {
		result *= 10;
	}
	return result;
}

//! Rescales ticks between decimal precisions, flooring when precision is lost
static int64_t ScaleTicks(int64_t ticks, idx_t from_precision, idx_t to_precision) {
	if (from_precision == to_precision) {
		return ticks;
	}
	if (from_precision < to_precision) {
		return ticks * PowerOfTen(to_precision - from_precision);
	}
	auto divisor = PowerOfTen(from_precision - to_precision);
	auto result = ticks / divisor;
	if (ticks % divisor != 0 && ticks < 0) {
		result -= 1;
	}
	return result;
}

template <class T>
static void ConvertNumeric(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                            const string &column_name) {
	auto typed = column->As<ch::ColumnVector<T>>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto &data = typed->GetWritableData();
	memcpy(FlatVector::GetData<T>(result), data.data() + offset, count * sizeof(T));
}

static void ConvertBool(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                         const string &column_name) {
	auto data = FlatVector::GetData<bool>(result);
	if (auto typed = column->As<ch::ColumnBool>()) {
		for (idx_t i = 0; i < count; i++) {
			data[i] = typed->At(offset + i);
		}
		return;
	}
	if (auto typed = column->As<ch::ColumnUInt8>()) {
		for (idx_t i = 0; i < count; i++) {
			data[i] = typed->At(offset + i) != 0;
		}
		return;
	}
	ThrowUnexpectedColumn(column, result, column_name);
}

static hugeint_t ToHugeint(const ch::Int128 &value) {
	return hugeint_t(absl::Int128High64(value), absl::Int128Low64(value));
}

static void ConvertInt128(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                           const string &column_name) {
	auto typed = column->As<ch::ColumnInt128>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto data = FlatVector::GetData<hugeint_t>(result);
	for (idx_t i = 0; i < count; i++) {
		data[i] = ToHugeint(typed->At(offset + i));
	}
}

static void ConvertUInt128(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                            const string &column_name) {
	auto typed = column->As<ch::ColumnUInt128>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto data = FlatVector::GetData<uhugeint_t>(result);
	for (idx_t i = 0; i < count; i++) {
		auto value = typed->At(offset + i);
		data[i] = uhugeint_t(absl::Uint128High64(value), absl::Uint128Low64(value));
	}
}

template <class T>
static void ConvertDecimalTo(const std::shared_ptr<ch::ColumnDecimal> &typed, Vector &result, idx_t offset,
                              idx_t count) {
	auto data = FlatVector::GetData<T>(result);
	for (idx_t i = 0; i < count; i++) {
		data[i] = static_cast<T>(typed->At(offset + i));
	}
}

static void ConvertDecimal(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                            const string &column_name) {
	auto typed = column->As<ch::ColumnDecimal>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	switch (result.GetType().InternalType()) {
	case PhysicalType::INT16:
		ConvertDecimalTo<int16_t>(typed, result, offset, count);
		break;
	case PhysicalType::INT32:
		ConvertDecimalTo<int32_t>(typed, result, offset, count);
		break;
	case PhysicalType::INT64:
		ConvertDecimalTo<int64_t>(typed, result, offset, count);
		break;
	case PhysicalType::INT128: {
		auto data = FlatVector::GetData<hugeint_t>(result);
		for (idx_t i = 0; i < count; i++) {
			data[i] = ToHugeint(typed->At(offset + i));
		}
		break;
	}
	default:
		ThrowUnexpectedColumn(column, result, column_name);
	}
}

template <class COLUMN>
static void ConvertStringColumn(const std::shared_ptr<COLUMN> &typed, Vector &result, idx_t offset, idx_t count,
                                 const string &column_name) {
	auto data = FlatVector::GetData<string_t>(result);
	auto &validity = FlatVector::Validity(result);
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			continue;
		}
		auto value = typed->At(offset + i);
		if (!ClickhouseUtils::IsValidUtf8(value.data(), value.size())) {
			throw InvalidInputException("ClickHouse column \"%s\" contains a value that is not valid UTF-8. Use "
			                             "clickhouse_query() with hex() or base64Encode() to read it",
			                             column_name);
		}
		data[i] = StringVector::AddString(result, value.data(), value.size());
	}
}

static void ConvertString(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                           const string &column_name) {
	if (auto typed = column->As<ch::ColumnString>()) {
		ConvertStringColumn(typed, result, offset, count, column_name);
		return;
	}
	if (auto typed = column->As<ch::ColumnFixedString>()) {
		ConvertStringColumn(typed, result, offset, count, column_name);
		return;
	}
	ThrowUnexpectedColumn(column, result, column_name);
}

static void ConvertDate(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                         const string &column_name) {
	auto data = FlatVector::GetData<date_t>(result);
	if (auto typed = column->As<ch::ColumnDate>()) {
		for (idx_t i = 0; i < count; i++) {
			data[i] = date_t(static_cast<int32_t>(typed->RawAt(offset + i)));
		}
		return;
	}
	if (auto typed = column->As<ch::ColumnDate32>()) {
		for (idx_t i = 0; i < count; i++) {
			data[i] = date_t(typed->RawAt(offset + i));
		}
		return;
	}
	ThrowUnexpectedColumn(column, result, column_name);
}

static void ConvertTimestamp(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                              const string &column_name) {
	auto data = FlatVector::GetData<timestamp_tz_t>(result);
	if (auto typed = column->As<ch::ColumnDateTime>()) {
		for (idx_t i = 0; i < count; i++) {
			auto seconds = static_cast<int64_t>(typed->RawAt(offset + i));
			data[i] = timestamp_tz_t(seconds * Interval::MICROS_PER_SEC);
		}
		return;
	}
	if (auto typed = column->As<ch::ColumnDateTime64>()) {
		auto precision = typed->GetPrecision();
		for (idx_t i = 0; i < count; i++) {
			data[i] = timestamp_tz_t(ScaleTicks(typed->At(offset + i), precision, 6));
		}
		return;
	}
	ThrowUnexpectedColumn(column, result, column_name);
}

static void ConvertTime(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                         const string &column_name) {
	auto time32 = column->As<ch::ColumnTime>();
	auto time64 = column->As<ch::ColumnTime64>();
	if (!time32 && !time64) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	idx_t precision = time32 ? 0 : time64->GetPrecision();
	auto nanoseconds = result.GetType().id() == LogicalTypeId::TIME_NS;
	auto max_ticks = Interval::SECS_PER_DAY * PowerOfTen(precision);
	auto &validity = FlatVector::Validity(result);
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			continue;
		}
		int64_t ticks = time32 ? static_cast<int64_t>(time32->At(offset + i)) : time64->At(offset + i);
		if (ticks < 0 || ticks > max_ticks) {
			throw ConversionException(
			    "ClickHouse column \"%s\" contains a time outside the DuckDB TIME range (00:00:00 - 24:00:00)",
			    column_name);
		}
		if (nanoseconds) {
			FlatVector::GetData<dtime_ns_t>(result)[i] = dtime_ns_t(ScaleTicks(ticks, precision, 9));
		} else {
			FlatVector::GetData<dtime_t>(result)[i] = dtime_t(ScaleTicks(ticks, precision, 6));
		}
	}
}

static void ConvertUUID(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                         const string &column_name) {
	auto typed = column->As<ch::ColumnUUID>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto data = FlatVector::GetData<hugeint_t>(result);
	for (idx_t i = 0; i < count; i++) {
		auto value = typed->At(offset + i);
		data[i] = UUID::FromUHugeint(uhugeint_t(value.first, value.second));
	}
}

template <class T>
static void WriteEnumPositions(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                                const unordered_map<int16_t, uint32_t> &positions, const string &column_name) {
	auto data = FlatVector::GetData<T>(result);
	auto &validity = FlatVector::Validity(result);
	auto enum8 = column->As<ch::ColumnEnum8>();
	auto enum16 = column->As<ch::ColumnEnum16>();
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			data[i] = 0;
			continue;
		}
		int16_t value = enum8 ? static_cast<int16_t>(enum8->At(offset + i)) : enum16->At(offset + i);
		auto position = positions.find(value);
		if (position == positions.end()) {
			throw ConversionException("ClickHouse column \"%s\" contains unknown enum value %d", column_name,
			                           static_cast<int32_t>(value));
		}
		data[i] = static_cast<T>(position->second);
	}
}

static void ConvertEnum(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                         const string &column_name) {
	if (!column->As<ch::ColumnEnum8>() && !column->As<ch::ColumnEnum16>()) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	// map ClickHouse enum values to positions in the DuckDB ENUM dictionary (matched by label)
	auto enum_type = column->Type()->As<ch::EnumType>();
	unordered_map<int16_t, uint32_t> positions;
	for (auto it = enum_type->BeginValueToName(); it != enum_type->EndValueToName(); ++it) {
		auto position = EnumType::GetPos(result.GetType(), string_t(it->second.data(), it->second.size()));
		if (position < 0) {
			ThrowUnexpectedColumn(column, result, column_name);
		}
		positions[it->first] = static_cast<uint32_t>(position);
	}
	switch (result.GetType().InternalType()) {
	case PhysicalType::UINT8:
		WriteEnumPositions<uint8_t>(column, result, offset, count, positions, column_name);
		break;
	case PhysicalType::UINT16:
		WriteEnumPositions<uint16_t>(column, result, offset, count, positions, column_name);
		break;
	case PhysicalType::UINT32:
		WriteEnumPositions<uint32_t>(column, result, offset, count, positions, column_name);
		break;
	default:
		ThrowUnexpectedColumn(column, result, column_name);
	}
}

//! Array(T) -> LIST(T); Array(Tuple(K, V)) -> MAP(K, V), which has the same physical layout
static void ConvertList(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                         const string &column_name) {
	auto array = column->As<ch::ColumnArray>();
	if (!array) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto entries = FlatVector::GetData<list_entry_t>(result);
	idx_t child_start = count == 0 ? 0 : array->GetOffset(offset);
	idx_t total = 0;
	for (idx_t i = 0; i < count; i++) {
		auto size = array->GetSize(offset + i);
		entries[i].offset = total;
		entries[i].length = size;
		total += size;
	}
	ListVector::Reserve(result, total);
	ListVector::SetListSize(result, total);
	if (total > 0) {
		ClickhouseConversion::ConvertColumn(array->GetData(), ListVector::GetEntry(result), child_start, total,
		                                     column_name);
	}
}

static void ConvertStruct(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                           const string &column_name) {
	auto tuple = column->As<ch::ColumnTuple>();
	auto &children = StructVector::GetEntries(result);
	if (!tuple || tuple->TupleSize() != children.size()) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	for (idx_t c = 0; c < children.size(); c++) {
		ClickhouseConversion::ConvertColumn(tuple->At(c), *children[c], offset, count, column_name);
	}
}

void ClickhouseConversion::ConvertColumn(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                                          const string &column_name) {
	D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
	auto values = column;
	auto &validity = FlatVector::Validity(result);
	if (auto nullable = column->As<ch::ColumnNullable>()) {
		for (idx_t i = 0; i < count; i++) {
			validity.Set(i, !nullable->IsNull(offset + i));
		}
		values = nullable->Nested();
	}
	if (values->As<ch::ColumnNothing>()) {
		for (idx_t i = 0; i < count; i++) {
			validity.SetInvalid(i);
		}
		return;
	}
	switch (result.GetType().id()) {
	case LogicalTypeId::BOOLEAN:
		ConvertBool(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::TINYINT:
		ConvertNumeric<int8_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::SMALLINT:
		ConvertNumeric<int16_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::INTEGER:
		ConvertNumeric<int32_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::BIGINT:
		ConvertNumeric<int64_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UTINYINT:
		ConvertNumeric<uint8_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::USMALLINT:
		ConvertNumeric<uint16_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UINTEGER:
		ConvertNumeric<uint32_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UBIGINT:
		ConvertNumeric<uint64_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::FLOAT:
		ConvertNumeric<float>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::DOUBLE:
		ConvertNumeric<double>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::HUGEINT:
		ConvertInt128(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UHUGEINT:
		ConvertUInt128(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::DECIMAL:
		ConvertDecimal(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::VARCHAR:
		ConvertString(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::DATE:
		ConvertDate(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
		ConvertTimestamp(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
		ConvertTime(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UUID:
		ConvertUUID(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::ENUM:
		ConvertEnum(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		ConvertList(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::STRUCT:
		ConvertStruct(values, result, offset, count, column_name);
		break;
	default:
		ThrowUnexpectedColumn(values, result, column_name);
	}
}

void ClickhouseConversion::ConvertBlock(const ch::Block &block, DataChunk &output, idx_t offset, idx_t count,
                                         const vector<string> &column_names) {
	if (output.ColumnCount() == 0) {
		// no columns requested (e.g. count(*)): only the row count matters
		output.SetCardinality(count);
		return;
	}
	if (block.GetColumnCount() != output.ColumnCount()) {
		throw InternalException("ClickHouse returned %llu columns, expected %llu",
		                         static_cast<uint64_t>(block.GetColumnCount()),
		                         static_cast<uint64_t>(output.ColumnCount()));
	}
	for (idx_t c = 0; c < output.ColumnCount(); c++) {
		ConvertColumn(block[c], output.data[c], offset, count, column_names[c]);
	}
	output.SetCardinality(count);
}

} // namespace duckdb
