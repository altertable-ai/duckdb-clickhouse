#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! Looks up the symbolic name ClickHouse itself uses for a native-protocol error code (e.g. 60 ->
//! "UNKNOWN_TABLE"). Returns an empty string for an unrecognized code.
//!
//! The native protocol's Exception.display_text does NOT include this name (verified directly against a
//! running ClickHouse 25.8 server: display_text is just "DB::Exception: <message>", never "... (NAME)").
//! The "(NAME)" annotation seen in clickhouse-client's terminal output is synthesized by the CLI itself
//! from the numeric code using its own compiled-in table; clickhouse-cpp's wire-level Exception struct
//! doesn't expose one. This table is that same mapping, so ClickhouseConnection can report errors the
//! same way clickhouse-client does ("ClickHouse error 60 (UNKNOWN_TABLE)") instead of just the exception
//! class name ("ClickHouse error 60 (DB::Exception)").
const char *ClickhouseErrorCodeName(int32_t code);

} // namespace duckdb
