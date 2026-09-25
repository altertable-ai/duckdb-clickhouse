#include "clickhouse_writer.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"

#include <clickhouse/columns/array.h>
#include <clickhouse/columns/bool.h>
#include <clickhouse/columns/date.h>
#include <clickhouse/columns/decimal.h>
#include <clickhouse/columns/enum.h>
#include <clickhouse/columns/factory.h>
#include <clickhouse/columns/lowcardinality.h>
#include <clickhouse/columns/map.h>
#include <clickhouse/columns/nullable.h>
#include <clickhouse/columns/numeric.h>
#include <clickhouse/columns/string.h>
#include <clickhouse/columns/time.h>
#include <clickhouse/columns/tuple.h>
#include <clickhouse/columns/uuid.h>

namespace duckdb {

namespace ch = clickhouse;

//===--------------------------------------------------------------------===//
// Write modes
//===--------------------------------------------------------------------===//
ClickhouseWriteMode ClickhouseWriter::GetWriteMode(const ClickhouseTypeNode &node) {
	static const unordered_set<string> NATIVE_TYPES = {
	    "Bool",   "Int8",     "Int16",      "Int32",   "Int64",   "UInt8",  "UInt16",      "UInt32",
	    "UInt64", "Int128",   "UInt128",    "Float32", "Float64", "String", "FixedString", "Date",
	    "Date32", "DateTime", "DateTime64", "Time",    "Time64",  "UUID",   "Enum8",       "Enum16"};
	static const unordered_set<string> CONVERTED_TYPES = {
	    "IPv4",  "IPv6", "Int256",     "UInt256",         "BFloat16", "JSON",
	    "Point", "Ring", "LineString", "MultiLineString", "Polygon",  "MultiPolygon"};
	auto &type = ClickhouseTypeWrappers::Of(node).base;
	auto &name = type.name;
	if (StringUtil::StartsWith(name, "Decimal")) {
		// Decimal(P <= 38) is read, and written, as a DuckDB DECIMAL; wider ones go through text
		return ClickhouseTypes::ToDuckDB(type).type.id() == LogicalTypeId::DECIMAL
		           ? ClickhouseWriteMode::NATIVE
		           : ClickhouseWriteMode::SERVER_CONVERSION;
	}
	if (NATIVE_TYPES.find(name) != NATIVE_TYPES.end()) {
		return ClickhouseWriteMode::NATIVE;
	}
	if (CONVERTED_TYPES.find(name) != CONVERTED_TYPES.end()) {
		return ClickhouseWriteMode::SERVER_CONVERSION;
	}
	if ((name == "Array" && type.children.size() == 1) || (name == "Tuple" && !type.children.empty()) ||
	    (name == "Map" && type.children.size() == 2)) {
		// nested values are encoded here, element by element: every element type must be native
		for (auto &child : type.children) {
			if (GetWriteMode(child) != ClickhouseWriteMode::NATIVE) {
				return ClickhouseWriteMode::UNSUPPORTED;
			}
		}
		return ClickhouseWriteMode::NATIVE;
	}
	return ClickhouseWriteMode::UNSUPPORTED;
}

string ClickhouseWriter::ServerInputType(const ClickhouseTypeNode &node) {
	auto base = ClickhouseTypeWrappers::Of(node).base.name == "BFloat16" ? "Float32" : "String";
	return ClickhouseTypes::IsNullable(node) ? "Nullable(" + string(base) + ")" : string(base);
}

string ClickhouseWriter::ParseText(const ClickhouseTypeNode &type, const string &expr) {
	// geo types are read as WKT (wkt()); CAST cannot parse WKT, the readWKT* functions can
	static const unordered_map<string, string> WKT_READERS = {
	    {"Point", "readWKTPoint"},           {"Ring", "readWKTRing"},
	    {"LineString", "readWKTLineString"}, {"MultiLineString", "readWKTMultiLineString"},
	    {"Polygon", "readWKTPolygon"},       {"MultiPolygon", "readWKTMultiPolygon"}};
	auto reader = WKT_READERS.find(type.name);
	if (reader != WKT_READERS.end()) {
		return reader->second + "(" + expr + ")";
	}
	auto cast = "CAST(" + expr + " AS " + type.text + ")";
	string valid;
	string expected;
	if (type.name == "Int256" || type.name == "UInt256") {
		// CAST wraps text outside the range around (2^256 is 0 as a UInt256): the value must print back as the text
		// without its + sign and leading zeros
		auto canonical =
		    "if(match(" + expr + R"(, '^[+-]?0+$'), '0', replaceRegexpOne()" + expr + R"(, '^\\+?(-?)0*', '\\1')))";
		valid = "match(" + expr + R"(, '^[+-]?[0-9]+$') AND toString()" + cast + ") = " + canonical;
		expected = "an optionally signed integer within its range";
	} else if (ClickhouseTypes::DecimalScale(type).IsValid()) {
		// CAST truncates the digits beyond the scale ('1.009' is 1.00 in a Decimal(76, 2)) and fails on too many
		// before the point
		auto scale = to_string(ClickhouseTypes::DecimalScale(type).GetIndex());
		valid = "match(" + expr + R"(, '^[+-]?([0-9]+\\.?|[0-9]*\\.[0-9]+)$') AND NOT match()" + expr +
		        R"(, '\\.[0-9]{)" + scale + R"(}[0-9]*[1-9]'))";
		expected = "a number in plain decimal notation with at most " + scale + " digit(s) after the point";
	} else {
		return cast;
	}
	auto message = "Cannot convert text to " + type.text + " exactly: expected " + expected;
	return "tupleElement(tuple(throwIf(NOT (" + valid + "), " + ClickhouseUtils::QuoteLiteral(message) + "), " + cast +
	       "), 2)";
}

string ClickhouseWriter::ServerConversion(const ClickhouseTypeNode &node, const string &expr) {
	auto wrappers = ClickhouseTypeWrappers::Of(node);
	if (wrappers.base.name == "BFloat16") {
		// from Float32: nothing to parse
		return "CAST(" + expr + " AS " + node.text + ")";
	}
	if (!wrappers.nullable) {
		return ParseText(wrappers.base, expr);
	}
	// CAST(Nullable(String) AS Nullable(IPv4)) turns text that does not parse into NULL: only the NULLs skip the
	// conversion (short_circuit_function_evaluation, see ClickhouseConnection::ExtensionQuerySettings, keeps it from
	// running on the empty string under a NULL)
	return "if(isNull(" + expr + "), NULL, " + ParseText(wrappers.base, "assumeNotNull(" + expr + ")") + ")";
}

//===--------------------------------------------------------------------===//
// Errors
//===--------------------------------------------------------------------===//
//! Not InternalException: a mismatch here almost always means the cached column type is stale (another connection
//! changed it since it was cached), and InternalException would invalidate the whole DuckDB instance for every
//! later query in DuckDB v1.5.4
[[noreturn]] static void ThrowMismatch(const Vector &source, const ch::ColumnRef &target, const string &column_name) {
	throw InvalidInputException(
	    "Cannot write DuckDB %s values into ClickHouse column \"%s\" of type %s: the table changed since its "
	    "metadata was cached; run CALL clickhouse_clear_cache() and retry",
	    source.GetType().ToString(), column_name, target->Type()->GetName());
}

[[noreturn]] static void ThrowNull(const ch::ColumnRef &target, const string &column_name) {
	throw ConversionException("Cannot insert NULL into ClickHouse column \"%s\": its type %s is not Nullable",
	                          column_name, target->Type()->GetName());
}

[[noreturn]] static void ThrowOutOfRange(const string &value, const ch::ColumnRef &target, const string &column_name) {
	throw ConversionException(
	    "Cannot insert %s into ClickHouse column \"%s\": it is outside the range of ClickHouse type %s", value,
	    column_name, target->Type()->GetName());
}

//===--------------------------------------------------------------------===//
// Appending
//===--------------------------------------------------------------------===//
//! The rows being appended: rows sel[0..count) of `source`, a vector holding `size` rows
struct AppendInput {
	Vector &source;
	idx_t size;
	const SelectionVector &sel;
	idx_t count;
	//! Append a default value for NULL rows instead of throwing: their NULL flags live in an enclosing Nullable
	bool nulls_as_default;
	const string &column_name;
};

static void AppendRows(const AppendInput &input, const ch::ColumnRef &target);

//! Calls append(value) for every non-NULL row and append_default() for every NULL row (or throws, see
//! AppendInput::nulls_as_default)
template <class T, class APPEND, class APPEND_DEFAULT>
static void ForEachValue(const AppendInput &input, const ch::ColumnRef &target, APPEND &&append,
                         APPEND_DEFAULT &&append_default) {
	UnifiedVectorFormat format;
	input.source.ToUnifiedFormat(input.size, format);
	auto data = UnifiedVectorFormat::GetData<T>(format);
	for (idx_t i = 0; i < input.count; i++) {
		auto index = format.sel->get_index(input.sel.get_index(i));
		if (!format.validity.RowIsValid(index)) {
			if (!input.nulls_as_default) {
				ThrowNull(target, input.column_name);
			}
			append_default();
			continue;
		}
		append(data[index]);
	}
}

template <class T>
static void AppendNumeric(const AppendInput &input, const ch::ColumnRef &target) {
	auto typed = target->As<ch::ColumnVector<T>>();
	if (!typed || input.source.GetType().InternalType() != GetTypeId<T>()) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	ForEachValue<T>(
	    input, target, [&](T value) { typed->Append(value); }, [&]() { typed->Append(T()); });
}

static void AppendBool(const AppendInput &input, const ch::ColumnRef &target) {
	if (input.source.GetType().id() != LogicalTypeId::BOOLEAN) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	if (auto typed = target->As<ch::ColumnBool>()) {
		ForEachValue<bool>(
		    input, target, [&](bool value) { typed->Append(value); }, [&]() { typed->Append(false); });
		return;
	}
	if (auto typed = target->As<ch::ColumnUInt8>()) {
		ForEachValue<bool>(
		    input, target, [&](bool value) { typed->Append(value ? 1 : 0); }, [&]() { typed->Append(0); });
		return;
	}
	ThrowMismatch(input.source, target, input.column_name);
}

static void AppendInt128(const AppendInput &input, const ch::ColumnRef &target) {
	auto typed = target->As<ch::ColumnInt128>();
	if (!typed || input.source.GetType().id() != LogicalTypeId::HUGEINT) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	ForEachValue<hugeint_t>(
	    input, target, [&](hugeint_t value) { typed->Append(absl::MakeInt128(value.upper, value.lower)); },
	    [&]() { typed->Append(ch::Int128(0)); });
}

static void AppendUInt128(const AppendInput &input, const ch::ColumnRef &target) {
	auto typed = target->As<ch::ColumnUInt128>();
	if (!typed || input.source.GetType().id() != LogicalTypeId::UHUGEINT) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	ForEachValue<uhugeint_t>(
	    input, target, [&](uhugeint_t value) { typed->Append(absl::MakeUint128(value.upper, value.lower)); },
	    [&]() { typed->Append(ch::UInt128(0)); });
}

static ch::Int128 ToInt128(int64_t value) {
	return ch::Int128(value);
}

static ch::Int128 ToInt128(hugeint_t value) {
	return absl::MakeInt128(value.upper, value.lower);
}

template <class T>
static void AppendDecimalValues(const AppendInput &input, const ch::ColumnRef &target, ch::ColumnDecimal &typed) {
	ForEachValue<T>(
	    input, target, [&](T value) { typed.Append(ToInt128(value)); }, [&]() { typed.Append(ch::Int128(0)); });
}

static void AppendDecimal(const AppendInput &input, const ch::ColumnRef &target) {
	auto typed = target->As<ch::ColumnDecimal>();
	auto &type = input.source.GetType();
	// DuckDB already cast the values to the column's DECIMAL(P, S): the unscaled integers are ClickHouse's too
	if (!typed || type.id() != LogicalTypeId::DECIMAL || DecimalType::GetScale(type) != typed->GetScale()) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	switch (type.InternalType()) {
	case PhysicalType::INT16:
		AppendDecimalValues<int16_t>(input, target, *typed);
		break;
	case PhysicalType::INT32:
		AppendDecimalValues<int32_t>(input, target, *typed);
		break;
	case PhysicalType::INT64:
		AppendDecimalValues<int64_t>(input, target, *typed);
		break;
	case PhysicalType::INT128:
		AppendDecimalValues<hugeint_t>(input, target, *typed);
		break;
	default:
		ThrowMismatch(input.source, target, input.column_name);
	}
}

static void AppendString(const AppendInput &input, const ch::ColumnRef &target) {
	auto typed = target->As<ch::ColumnString>();
	if (!typed || input.source.GetType().InternalType() != PhysicalType::VARCHAR) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	ForEachValue<string_t>(
	    input, target, [&](string_t value) { typed->Append(std::string_view(value.GetData(), value.GetSize())); },
	    [&]() { typed->Append(std::string_view()); });
}

static void AppendFixedString(const AppendInput &input, const ch::ColumnRef &target) {
	auto typed = target->As<ch::ColumnFixedString>();
	if (!typed || input.source.GetType().InternalType() != PhysicalType::VARCHAR) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	auto size = typed->FixedSize();
	ForEachValue<string_t>(
	    input, target,
	    [&](string_t value) {
		    if (value.GetSize() > size) {
			    throw ConversionException(
			        "Cannot insert a %d-byte string into ClickHouse column \"%s\": it does not fit ClickHouse type %s",
			        value.GetSize(), input.column_name, target->Type()->GetName());
		    }
		    // shorter values are zero-padded by clickhouse-cpp
		    typed->Append(std::string_view(value.GetData(), value.GetSize()));
	    },
	    [&]() { typed->Append(std::string_view()); });
}

static void AppendDate(const AppendInput &input, const ch::ColumnRef &target) {
	if (input.source.GetType().id() != LogicalTypeId::DATE) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	if (auto typed = target->As<ch::ColumnDate>()) {
		// days since 1970-01-01 as UInt16: 1970-01-01 to 2149-06-06
		ForEachValue<date_t>(
		    input, target,
		    [&](date_t value) {
			    if (value.days < 0 || value.days > NumericLimits<uint16_t>::Maximum()) {
				    ThrowOutOfRange(Date::ToString(value), target, input.column_name);
			    }
			    typed->AppendRaw(static_cast<uint16_t>(value.days));
		    },
		    [&]() { typed->AppendRaw(0); });
		return;
	}
	if (auto typed = target->As<ch::ColumnDate32>()) {
		static const auto MIN_DATE = Date::FromDate(1900, 1, 1);
		static const auto MAX_DATE = Date::FromDate(2299, 12, 31);
		ForEachValue<date_t>(
		    input, target,
		    [&](date_t value) {
			    if (value < MIN_DATE || value > MAX_DATE) {
				    ThrowOutOfRange(Date::ToString(value), target, input.column_name);
			    }
			    typed->AppendRaw(value.days);
		    },
		    [&]() { typed->AppendRaw(0); });
		return;
	}
	ThrowMismatch(input.source, target, input.column_name);
}

//! Decimal digits of a second in a DuckDB timestamp type's int64 ticks; false for non-timestamp types
static bool TimestampPrecision(LogicalTypeId id, idx_t &precision) {
	switch (id) {
	case LogicalTypeId::TIMESTAMP_SEC:
		precision = 0;
		return true;
	case LogicalTypeId::TIMESTAMP_MS:
		precision = 3;
		return true;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		precision = 6;
		return true;
	case LogicalTypeId::TIMESTAMP_NS:
		precision = 9;
		return true;
	default:
		return false;
	}
}

//! `ticks` at `from` digits rescaled to `to` digits (floored); false when that overflows int64
static bool TryRescaleTicks(int64_t ticks, idx_t from, idx_t to, int64_t &result) {
	if (to <= from) {
		result = ClickhouseUtils::ScaleTicks(ticks, from, to);
		return true;
	}
	return TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(ticks, ClickhouseUtils::PowerOfTen(to - from),
	                                                                 result);
}

//! The value, for errors; infinities and values beyond int64 microseconds have no timestamp text
static string TimestampText(int64_t ticks, idx_t precision) {
	int64_t micros;
	if (!TryRescaleTicks(ticks, precision, 6, micros) || !Timestamp::IsFinite(timestamp_t(micros))) {
		return "an infinite or out-of-range timestamp";
	}
	return Timestamp::ToString(timestamp_t(micros)) + " (UTC)";
}

static void AppendTimestamp(const AppendInput &input, const ch::ColumnRef &target) {
	idx_t source_precision;
	if (!TimestampPrecision(input.source.GetType().id(), source_precision)) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	if (auto typed = target->As<ch::ColumnDateTime>()) {
		// seconds since the epoch as UInt32: 1970-01-01 00:00:00 to 2106-02-07 06:28:15
		ForEachValue<int64_t>(
		    input, target,
		    [&](int64_t ticks) {
			    auto seconds = ClickhouseUtils::ScaleTicks(ticks, source_precision, 0);
			    if (seconds < 0 || seconds > NumericLimits<uint32_t>::Maximum()) {
				    ThrowOutOfRange(TimestampText(ticks, source_precision), target, input.column_name);
			    }
			    typed->AppendRaw(static_cast<uint32_t>(seconds));
		    },
		    [&]() { typed->AppendRaw(0); });
		return;
	}
	if (auto typed = target->As<ch::ColumnDateTime64>()) {
		static const auto MIN_MICROS = Timestamp::FromDatetime(Date::FromDate(1900, 1, 1), dtime_t(0)).value;
		static const auto MAX_MICROS =
		    Timestamp::FromDatetime(Date::FromDate(2299, 12, 31), Time::FromTime(23, 59, 59, 999999)).value;
		auto target_precision = typed->GetPrecision();
		ForEachValue<int64_t>(
		    input, target,
		    [&](int64_t ticks) {
			    int64_t micros;
			    int64_t result;
			    if (!TryRescaleTicks(ticks, source_precision, 6, micros) || micros < MIN_MICROS ||
			        micros > MAX_MICROS || !TryRescaleTicks(ticks, source_precision, target_precision, result)) {
				    ThrowOutOfRange(TimestampText(ticks, source_precision), target, input.column_name);
			    }
			    typed->Append(result);
		    },
		    [&]() { typed->Append(0); });
		return;
	}
	ThrowMismatch(input.source, target, input.column_name);
}

static void AppendTime(const AppendInput &input, const ch::ColumnRef &target) {
	auto id = input.source.GetType().id();
	auto time32 = target->As<ch::ColumnTime>();
	auto time64 = target->As<ch::ColumnTime64>();
	if ((id != LogicalTypeId::TIME && id != LogicalTypeId::TIME_NS) || (!time32 && !time64)) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	idx_t source_precision = id == LogicalTypeId::TIME ? 6 : 9;
	idx_t target_precision = time32 ? 0 : time64->GetPrecision();
	// dtime_t and dtime_ns_t are both a single int64_t tick count; DuckDB times (00:00:00 - 24:00:00) always fit
	ForEachValue<int64_t>(
	    input, target,
	    [&](int64_t ticks) {
		    auto scaled = ClickhouseUtils::ScaleTicks(ticks, source_precision, target_precision);
		    if (time32) {
			    time32->Append(static_cast<int32_t>(scaled));
		    } else {
			    time64->Append(scaled);
		    }
	    },
	    [&]() {
		    if (time32) {
			    time32->Append(0);
		    } else {
			    time64->Append(0);
		    }
	    });
}

static void AppendUUID(const AppendInput &input, const ch::ColumnRef &target) {
	auto typed = target->As<ch::ColumnUUID>();
	if (!typed || input.source.GetType().id() != LogicalTypeId::UUID) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	ForEachValue<hugeint_t>(
	    input, target,
	    [&](hugeint_t value) {
		    auto bits = UUID::ToUHugeint(value);
		    typed->Append(ch::UUID(bits.upper, bits.lower));
	    },
	    [&]() { typed->Append(ch::UUID(0, 0)); });
}

template <class INDEX, class VALUE>
static void AppendEnumValues(const AppendInput &input, const ch::ColumnRef &target, ch::ColumnEnum<VALUE> &typed) {
	auto enum_type = target->Type()->As<ch::EnumType>();
	auto &source_type = input.source.GetType();
	// a NULL row still needs a valid enum value underneath its NULL flag
	auto default_value = static_cast<VALUE>(enum_type->BeginValueToName()->first);
	ForEachValue<INDEX>(
	    input, target,
	    [&](INDEX index) {
		    auto label = EnumType::GetString(source_type, index).GetString();
		    if (!enum_type->HasEnumName(label)) {
			    throw ConversionException(
			        "Cannot insert \"%s\" into ClickHouse column \"%s\": it is not a label of ClickHouse type %s",
			        label, input.column_name, target->Type()->GetName());
		    }
		    typed.Append(label);
	    },
	    [&]() { typed.Append(default_value); });
}

template <class INDEX>
static void AppendEnumIndexes(const AppendInput &input, const ch::ColumnRef &target) {
	if (auto enum8 = target->As<ch::ColumnEnum8>()) {
		AppendEnumValues<INDEX>(input, target, *enum8);
		return;
	}
	if (auto enum16 = target->As<ch::ColumnEnum16>()) {
		AppendEnumValues<INDEX>(input, target, *enum16);
		return;
	}
	ThrowMismatch(input.source, target, input.column_name);
}

static void AppendEnum(const AppendInput &input, const ch::ColumnRef &target) {
	auto &type = input.source.GetType();
	if (type.id() != LogicalTypeId::ENUM) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	switch (type.InternalType()) {
	case PhysicalType::UINT8:
		AppendEnumIndexes<uint8_t>(input, target);
		break;
	case PhysicalType::UINT16:
		AppendEnumIndexes<uint16_t>(input, target);
		break;
	case PhysicalType::UINT32:
		AppendEnumIndexes<uint32_t>(input, target);
		break;
	default:
		ThrowMismatch(input.source, target, input.column_name);
	}
}

//! Appends LIST (or MAP) rows to `array`: every element to array.GetData(), then one end offset per row
static void AppendListRows(const AppendInput &input, const ch::ColumnRef &target, ch::ColumnArray &array) {
	UnifiedVectorFormat format;
	input.source.ToUnifiedFormat(input.size, format);
	auto entries = UnifiedVectorFormat::GetData<list_entry_t>(format);
	idx_t total = 0;
	for (idx_t i = 0; i < input.count; i++) {
		auto index = format.sel->get_index(input.sel.get_index(i));
		if (!format.validity.RowIsValid(index)) {
			if (!input.nulls_as_default) {
				ThrowNull(target, input.column_name);
			}
			continue;
		}
		total += entries[index].length;
	}
	auto data = array.GetData();
	auto end = data->Size();
	if (total > 0) {
		SelectionVector element_sel(total);
		idx_t position = 0;
		for (idx_t i = 0; i < input.count; i++) {
			auto index = format.sel->get_index(input.sel.get_index(i));
			if (!format.validity.RowIsValid(index)) {
				continue;
			}
			auto &entry = entries[index];
			for (idx_t element = entry.offset; element < entry.offset + entry.length; element++) {
				element_sel.set_index(position++, element);
			}
		}
		AppendInput elements {ListVector::GetEntry(input.source),
		                      ListVector::GetListSize(input.source),
		                      element_sel,
		                      total,
		                      false,
		                      input.column_name};
		AppendRows(elements, data);
	}
	// ColumnArray offsets are absolute end positions in its data column; a NULL placeholder row is empty
	for (idx_t i = 0; i < input.count; i++) {
		auto index = format.sel->get_index(input.sel.get_index(i));
		if (format.validity.RowIsValid(index)) {
			end += entries[index].length;
		}
		array.OffsetsIncrease(end);
	}
}

static void AppendList(const AppendInput &input, const ch::ColumnRef &target) {
	auto array = target->As<ch::ColumnArray>();
	if (!array || input.source.GetType().id() != LogicalTypeId::LIST) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	AppendListRows(input, target, *array);
}

static void AppendMap(const AppendInput &input, const ch::ColumnRef &target) {
	auto map_type = target->Type()->As<ch::MapType>();
	if (!map_type || input.source.GetType().id() != LogicalTypeId::MAP) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	// a DuckDB MAP is a LIST of (key, value) STRUCTs, ClickHouse's Map an Array(Tuple(K, V)). ColumnMap does not
	// expose that array, so fill a new one and append it wrapped in a ColumnMap
	auto data = ch::CreateColumnByType("Array(Tuple(" + map_type->GetKeyType()->GetName() + ", " +
	                                   map_type->GetValueType()->GetName() + "))");
	AppendListRows(input, target, *data->As<ch::ColumnArray>());
	target->Append(std::make_shared<ch::ColumnMap>(data));
}

static void AppendStruct(const AppendInput &input, const ch::ColumnRef &target) {
	auto tuple = target->As<ch::ColumnTuple>();
	if (!tuple || input.source.GetType().id() != LogicalTypeId::STRUCT) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	// struct children can only be addressed through a flat struct vector
	input.source.Flatten(input.size);
	auto &children = StructVector::GetEntries(input.source);
	if (children.size() != tuple->TupleSize()) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	if (!input.nulls_as_default) {
		auto &validity = FlatVector::Validity(input.source);
		for (idx_t i = 0; i < input.count; i++) {
			if (!validity.RowIsValid(input.sel.get_index(i))) {
				ThrowNull(target, input.column_name);
			}
		}
	}
	for (idx_t c = 0; c < children.size(); c++) {
		AppendInput child {*children[c], input.size, input.sel, input.count, input.nulls_as_default, input.column_name};
		AppendRows(child, tuple->At(c));
	}
}

static void AppendNullable(const AppendInput &input, const ch::ColumnRef &target) {
	auto nullable = target->As<ch::ColumnNullable>();
	UnifiedVectorFormat format;
	input.source.ToUnifiedFormat(input.size, format);
	for (idx_t i = 0; i < input.count; i++) {
		nullable->Append(!format.validity.RowIsValid(format.sel->get_index(input.sel.get_index(i))));
	}
	AppendInput values {input.source, input.size, input.sel, input.count, true, input.column_name};
	AppendRows(values, nullable->Nested());
}

static void AppendLowCardinality(const AppendInput &input, const ch::ColumnRef &target) {
	// only reached if the server sends LowCardinality in an INSERT header despite
	// low_cardinality_allow_in_native_format=0: fill a plain column, then dictionary-encode it
	auto low_cardinality_type = target->Type()->As<ch::LowCardinalityType>();
	auto values = ch::CreateColumnByType(low_cardinality_type->GetNestedType()->GetName());
	AppendRows(input, values);
	if (auto nullable_values = values->As<ch::ColumnNullable>()) {
		target->Append(std::make_shared<ch::ColumnLowCardinality>(nullable_values));
	} else {
		target->Append(std::make_shared<ch::ColumnLowCardinality>(values));
	}
}

static void AppendRows(const AppendInput &input, const ch::ColumnRef &target) {
	switch (target->Type()->GetCode()) {
	case ch::Type::Nullable:
		AppendNullable(input, target);
		break;
	case ch::Type::LowCardinality:
		AppendLowCardinality(input, target);
		break;
	case ch::Type::Bool:
		AppendBool(input, target);
		break;
	case ch::Type::Int8:
		AppendNumeric<int8_t>(input, target);
		break;
	case ch::Type::Int16:
		AppendNumeric<int16_t>(input, target);
		break;
	case ch::Type::Int32:
		AppendNumeric<int32_t>(input, target);
		break;
	case ch::Type::Int64:
		AppendNumeric<int64_t>(input, target);
		break;
	case ch::Type::UInt8:
		// some servers describe Bool columns as UInt8
		if (input.source.GetType().id() == LogicalTypeId::BOOLEAN) {
			AppendBool(input, target);
		} else {
			AppendNumeric<uint8_t>(input, target);
		}
		break;
	case ch::Type::UInt16:
		AppendNumeric<uint16_t>(input, target);
		break;
	case ch::Type::UInt32:
		AppendNumeric<uint32_t>(input, target);
		break;
	case ch::Type::UInt64:
		AppendNumeric<uint64_t>(input, target);
		break;
	case ch::Type::Float32:
		AppendNumeric<float>(input, target);
		break;
	case ch::Type::Float64:
		AppendNumeric<double>(input, target);
		break;
	case ch::Type::Int128:
		AppendInt128(input, target);
		break;
	case ch::Type::UInt128:
		AppendUInt128(input, target);
		break;
	case ch::Type::Decimal:
	case ch::Type::Decimal32:
	case ch::Type::Decimal64:
	case ch::Type::Decimal128:
		AppendDecimal(input, target);
		break;
	case ch::Type::String:
		AppendString(input, target);
		break;
	case ch::Type::FixedString:
		AppendFixedString(input, target);
		break;
	case ch::Type::Date:
	case ch::Type::Date32:
		AppendDate(input, target);
		break;
	case ch::Type::DateTime:
	case ch::Type::DateTime64:
		AppendTimestamp(input, target);
		break;
	case ch::Type::Time:
	case ch::Type::Time64:
		AppendTime(input, target);
		break;
	case ch::Type::UUID:
		AppendUUID(input, target);
		break;
	case ch::Type::Enum8:
	case ch::Type::Enum16:
		AppendEnum(input, target);
		break;
	case ch::Type::Array:
		AppendList(input, target);
		break;
	case ch::Type::Tuple:
		AppendStruct(input, target);
		break;
	case ch::Type::Map:
		AppendMap(input, target);
		break;
	default:
		throw NotImplementedException("Cannot insert into ClickHouse column \"%s\" of type %s", input.column_name,
		                              target->Type()->GetName());
	}
}

void ClickhouseWriter::AppendVector(Vector &source, idx_t count, const ch::ColumnRef &target,
                                    const string &column_name) {
	AppendInput input {source, count, *FlatVector::IncrementalSelectionVector(), count, false, column_name};
	AppendRows(input, target);
}

} // namespace duckdb
