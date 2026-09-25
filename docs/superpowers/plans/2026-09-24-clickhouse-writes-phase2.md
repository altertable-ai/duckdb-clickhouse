# ClickHouse writes — Phase 2 (DDL) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** DuckDB DDL on attached ClickHouse databases:
- `CREATE TABLE` (with `IF NOT EXISTS` and `OR REPLACE`) and `DROP TABLE`;
- `CREATE SCHEMA` and `DROP SCHEMA` (with `CASCADE`);
- `ALTER TABLE` to add, drop or rename a column, or rename the table;
- `CREATE TABLE … AS SELECT`.

**Architecture:**
- A reverse type mapping (`ClickhouseDdlTypes`) turns DuckDB column types into ClickHouse types.
- `ClickhouseDdl` builds the DDL SQL and runs it through `ClickhouseCatalog::StartWrite`. That marks the transaction as written. The statement runs with `enable_time_time64_type=1`, and afterwards the catalog cache is cleared.
- The catalog and schema-entry hooks call `ClickhouseDdl`.
- CTAS reuses the Phase 1 `ClickhouseInsert` sink in a "create first" mode: the table is created at execution time, then the rows stream in.

**Tech Stack:**
- C++17, DuckDB v1.5.4 (submodule `duckdb`)
- clickhouse-cpp 2.6.2 (vcpkg overlay port, patched)
- sqllogictest via `make smoke` against ClickHouse 25.8

**Spec:** `docs/superpowers/specs/2026-09-24-clickhouse-writes-design.md`. This plan implements Phase 2 (§10.2); read §2, §5, §8 and §9. Phase 1 is on the branch; its plan is `docs/superpowers/plans/2026-09-24-clickhouse-writes-phase1.md`.

## Global Constraints

- **Branch:** work on `su/init`. Commit there; never push.
- **Reverse type mapping** (spec §5). The DuckDB → ClickHouse pairs:

  | DuckDB | ClickHouse |
  |---|---|
  | BOOLEAN | Bool |
  | TINYINT … BIGINT | Int8 … Int64 |
  | UTINYINT … UBIGINT | UInt8 … UInt64 |
  | HUGEINT / UHUGEINT | Int128 / UInt128 |
  | FLOAT / DOUBLE | Float32 / Float64 |
  | DECIMAL(p, s) | Decimal(p, s) |
  | VARCHAR, BLOB | String |
  | JSON | JSON |
  | DATE | Date32 |
  | TIMESTAMP_S | DateTime('UTC') |
  | TIMESTAMP_MS | DateTime64(3, 'UTC') |
  | TIMESTAMP, TIMESTAMPTZ | DateTime64(6, 'UTC') |
  | TIMESTAMP_NS | DateTime64(9, 'UTC') |
  | TIME / TIME_NS | Time64(6) / Time64(9) |
  | UUID | UUID |
  | ENUM | Enum8 (≤ 127 labels) or Enum16, labels numbered 1…n in DuckDB order |
  | LIST(T) | Array(T) |
  | STRUCT | Tuple(name T, …) |
  | MAP(K, V) | Map(K, V) |

  Anything else → `NotImplementedException`.
- **Nullability:**
  - A nullable column becomes `Nullable(T)`.
  - Array, Tuple, Map and JSON are never wrapped, because ClickHouse can't make them Nullable.
  - Nested element and field types are always nullable, since a DuckDB LIST, STRUCT or MAP can hold NULLs. Map keys are never nullable.
  - PRIMARY KEY and NOT NULL columns are not Nullable.
- **CREATE TABLE** produces:

  ```sql
  CREATE [OR REPLACE] TABLE [IF NOT EXISTS] `db`.`t` (…) ENGINE = <ch_default_table_engine>
  ```

  - The statement adds `ORDER BY (<pk cols>)`, or `ORDER BY tuple()` without a primary key, only when the engine name ends in `MergeTree`.
  - DEFAULT values must fold to a constant. Anything else is rejected, with a hint to use `clickhouse_execute()`.
  - UNIQUE (non-primary-key), CHECK and FOREIGN KEY constraints, generated columns, and `PARTITIONED BY` / `SORTED BY` / `WITH` options are rejected with `NotImplementedException`.
- **New setting** `ch_default_table_engine`: VARCHAR, default `'MergeTree'`. It must be an identifier optionally followed by `(…)` that closes the string, e.g. `ReplicatedMergeTree('/p/{shard}', '{replica}')`. Invalid values are rejected when set: `Invalid ch_default_table_engine`.
- **DDL runs through `ClickhouseCatalog::StartWrite(context)`**, which checks READ_ONLY, takes a connection and marks the transaction as written. The statement is sent with the query setting `enable_time_time64_type=1`, which is not IMPORTANT, so older servers ignore it. Afterwards `ClickhouseCatalog::ClearCache()` is called, even when the statement failed.
- **Keep the calling schema entry alive across ClearCache.** `ClearCache()` frees cached schema entries. A `ClickhouseSchemaEntry` method that triggers it must first hold `auto keep_alive = catalog.GetSchemaEntryOwner(name);`, so that `this` stays alive.
- **DROP TABLE on a ClickHouse view is rejected.** This covers engines View, MaterializedView, LiveView, WindowView and Dictionary, and the error points to `clickhouse_execute()`. `DROP VIEW` can't be supported: DuckDB casts view-typed catalog lookups to `ViewCatalogEntry` (bind_simple.cpp, COMMENT ON COLUMN), so our table entries must never be returned for them. This deviates from spec §5; record it in the README.
- **Errors:** no `InternalException` on user-triggerable paths, because it invalidates the DuckDB instance. Server errors keep the `ClickHouse error <code> (<NAME>): …` format. Passwords never appear.
- **Every src/ object library** keeps `target_compile_features(<lib> PUBLIC cxx_std_17)`.
- **Tests:**
  - Server tests use `require-env CLICKHOUSE_TEST_HOST/PORT/USER/PASSWORD`.
  - Each file creates and drops its own ClickHouse database (`w_ddl`, `w_ddl2`, `w_ctas`), and never modifies the `test_db` / `other_db` fixtures.
  - Every DDL change is checked through DuckDB (`duckdb_tables()`, `duckdb_columns()`, `information_schema`) and through ClickHouse (`clickhouse_query` on `system.tables`, `system.columns`, `system.databases`).
- **Commit trailer:** use the Co-Authored-By trailer your own harness instructs you to use.

## Build & test cheat sheet

```bash
cd /Users/redox/dev/altertable-ai/duckdb-clickhouse
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja
make release && make smoke ARGS=test/sql/write/ddl_table.test    # focused
make smoke                                                        # all tests, fresh ClickHouse 25.8 container
make debug && make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*' # debug build (works since Phase 1's fix)
./build/release/test/unittest "test/*"                            # no-server suite
CLICKHOUSE_TEST_KEEP=1 make smoke ARGS=...                        # keep the container; docker rm -f clickhouse-scanner-test after
```
- Run long builds and tests in the background and poll every few minutes, printing progress. An agent that is silent for 10 minutes is killed.
- Check Docker with `timeout 20 docker info`. Never restart Docker Desktop. Remove only a leftover `clickhouse-scanner-test` container.

**Expected-output rule:** if a value differs only in formatting (e.g. how ClickHouse prints a type in `system.columns`), fix the expectation. If the value itself is different, fix the code. List every such change in your report.

## File Structure

```
src/include/clickhouse_ddl_types.hpp, src/clickhouse_ddl_types.cpp     NEW  ClickhouseDdlTypes::ToClickhouse (reverse type mapping)
src/include/storage/clickhouse_ddl.hpp, src/storage/clickhouse_ddl.cpp  NEW  ClickhouseDdl: SQL builders + Execute + table lookup
src/include/clickhouse_connection.hpp, src/clickhouse_connection.cpp    MOD  Execute(sql, query_settings)
src/include/storage/clickhouse_table_entry.hpp, src/storage/clickhouse_table_entry.cpp, src/storage/clickhouse_table_set.cpp  MOD  engine
src/storage/clickhouse_schema_entry.cpp                   MOD  CreateTable, DropEntry (Task 1), Alter (Task 2)
src/storage/clickhouse_catalog.cpp                        MOD  CreateSchema, DropSchema (Task 2), PlanCreateTableAs (Task 3)
src/include/storage/clickhouse_insert.hpp, src/storage/clickhouse_insert.cpp  MOD  CTAS mode (Task 3)
src/clickhouse_writer.cpp                                 MOD  naive TIMESTAMP family sources (Task 3)
src/clickhouse_scanner_extension.cpp                      MOD  ch_default_table_engine (Task 1)
src/CMakeLists.txt, src/storage/CMakeLists.txt            MOD  new sources (Task 1)
test/sql/write/ddl_table.test (T1), ddl_schema_alter.test (T2), ctas.test (T3)   NEW
test/sql/write/unsupported.test, test/sql/attach/attach.test, test/sql/write/read_only_attach.test  MOD
README.md                                                 MOD
```

## Task Order
1. Reverse type mapping, CREATE TABLE and DROP TABLE, the engine setting, constant DEFAULTs.
2. CREATE and DROP SCHEMA; ALTER TABLE (add, drop or rename a column; rename the table).
3. CREATE TABLE … AS SELECT.

Each task depends on the ones before it.

---

### Task 1: CREATE TABLE / DROP TABLE with the reverse type mapping

**Files:**
- Create: `src/include/clickhouse_ddl_types.hpp`, `src/clickhouse_ddl_types.cpp`
- Create: `src/include/storage/clickhouse_ddl.hpp`, `src/storage/clickhouse_ddl.cpp`
- Modify: `src/include/clickhouse_connection.hpp`, `src/clickhouse_connection.cpp`
- Modify: `src/include/storage/clickhouse_table_entry.hpp`, `src/storage/clickhouse_table_entry.cpp`, `src/storage/clickhouse_table_set.cpp`
- Modify: `src/storage/clickhouse_schema_entry.cpp`
- Modify: `src/clickhouse_scanner_extension.cpp`, `src/CMakeLists.txt`, `src/storage/CMakeLists.txt`
- Modify: `test/sql/write/unsupported.test`
- Create: `test/sql/write/ddl_table.test`
- Modify: `README.md`

**Interfaces:**
- Consumes (Phase 1):
  - `ClickhouseCatalog::StartWrite(ClientContext &) → ClickhousePoolConnection`
  - `ClickhouseCatalog::ClearCache()`
  - `ClickhouseCatalog::GetSchemaEntryOwner(const string &) → shared_ptr<CatalogEntry>`
  - `ClickhouseFilterPushdown::TransformConstant(const Value &) → string`
  - `ClickhouseUtils::QuoteIdentifier` / `QuoteLiteral`
- Produces:
  - `static string ClickhouseDdlTypes::ToClickhouse(const LogicalType &type, bool nullable)`
  - `bool ClickhouseConnection::Execute(const string &sql, const vector<std::pair<string, string>> &query_settings)` (overload)
  - `string ClickhouseTableEntry::GetEngine() const`
  - `class ClickhouseDdl` with static members:
    - `string CreateTableSql(ClientContext &, const string &database, CreateTableInfo &)`
    - `string ColumnSql(ClientContext &, const ColumnDefinition &, bool nullable)`
    - `string DefaultValueSql(ClientContext &, const ColumnDefinition &)`
    - `string TableEngine(ClientContext &)`
    - `void ValidateEngine(const string &)`
    - `void Execute(ClientContext &, ClickhouseCatalog &, const string &sql)`
    - `optional_ptr<ClickhouseTableEntry> LookupTable(ClientContext &, ClickhouseCatalog &, const string &database, const string &table)`
    - `ClickhouseTableEntry &CreateTable(ClientContext &, ClickhouseCatalog &, const string &database, CreateTableInfo &)`

- [ ] **Step 1: Write the failing test**

In `test/sql/write/unsupported.test`, **delete** the `CREATE TABLE ch.test_db.new_table (i INTEGER);` and `DROP TABLE ch.test_db.t1;` statement-error blocks. After this task they would really create a table in, and drop a table from, the shared fixture.

`test/sql/write/ddl_table.test`:
```
# name: test/sql/write/ddl_table.test
# description: CREATE TABLE and DROP TABLE on an attached ClickHouse database
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

require icu

require json

statement ok
SET TimeZone = 'UTC';

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE IF EXISTS w_ddl');

statement ok
CALL clickhouse_execute('ch', 'CREATE DATABASE w_ddl');

# ---------------------------------------------------------------------------
# every mapped type; PRIMARY KEY and NOT NULL columns are not Nullable
# ---------------------------------------------------------------------------
statement ok
CREATE TABLE ch.w_ddl.all_types (
    id INTEGER PRIMARY KEY,
    b BOOLEAN, i8 TINYINT, i16 SMALLINT, i64 BIGINT, u8 UTINYINT, u16 USMALLINT, u32 UINTEGER, u64 UBIGINT,
    h HUGEINT, uh UHUGEINT, f FLOAT, d DOUBLE, dec DECIMAL(18, 4), s VARCHAR NOT NULL, bl BLOB, dt DATE,
    ts_s TIMESTAMP_S, ts_ms TIMESTAMP_MS, ts TIMESTAMP, tstz TIMESTAMPTZ, ts_ns TIMESTAMP_NS,
    t TIME, u UUID, e ENUM('x', 'y'), l INTEGER[], st STRUCT(a INTEGER, b VARCHAR), m MAP(VARCHAR, DOUBLE), j JSON
);

query II
SELECT * FROM clickhouse_query('ch', 'SELECT name, type FROM system.columns WHERE database = ''w_ddl'' AND table = ''all_types'' ORDER BY position');
----
id	Int32
b	Nullable(Bool)
i8	Nullable(Int8)
i16	Nullable(Int16)
i64	Nullable(Int64)
u8	Nullable(UInt8)
u16	Nullable(UInt16)
u32	Nullable(UInt32)
u64	Nullable(UInt64)
h	Nullable(Int128)
uh	Nullable(UInt128)
f	Nullable(Float32)
d	Nullable(Float64)
dec	Nullable(Decimal(18, 4))
s	String
bl	Nullable(String)
dt	Nullable(Date32)
ts_s	Nullable(DateTime('UTC'))
ts_ms	Nullable(DateTime64(3, 'UTC'))
ts	Nullable(DateTime64(6, 'UTC'))
tstz	Nullable(DateTime64(6, 'UTC'))
ts_ns	Nullable(DateTime64(9, 'UTC'))
t	Nullable(Time64(6))
u	Nullable(UUID)
e	Nullable(Enum8('x' = 1, 'y' = 2))
l	Array(Nullable(Int32))
st	Tuple(a Nullable(Int32), b Nullable(String))
m	Map(String, Nullable(Float64))
j	JSON

query II
SELECT * FROM clickhouse_query('ch', 'SELECT engine, sorting_key FROM system.tables WHERE database = ''w_ddl'' AND name = ''all_types''');
----
MergeTree	id

# visible to DuckDB right away, with the read-path types
query II
SELECT column_name, data_type FROM information_schema.columns WHERE table_catalog = 'ch' AND table_schema = 'w_ddl' AND table_name = 'all_types' ORDER BY ordinal_position;
----
id	INTEGER
b	BOOLEAN
i8	TINYINT
i16	SMALLINT
i64	BIGINT
u8	UTINYINT
u16	USMALLINT
u32	UINTEGER
u64	UBIGINT
h	HUGEINT
uh	UHUGEINT
f	FLOAT
d	DOUBLE
dec	DECIMAL(18,4)
s	VARCHAR
bl	VARCHAR
dt	DATE
ts_s	TIMESTAMP WITH TIME ZONE
ts_ms	TIMESTAMP WITH TIME ZONE
ts	TIMESTAMP WITH TIME ZONE
tstz	TIMESTAMP WITH TIME ZONE
ts_ns	TIMESTAMP WITH TIME ZONE
t	TIME
u	UUID
e	ENUM('x', 'y')
l	INTEGER[]
st	STRUCT(a INTEGER, b VARCHAR)
m	MAP(VARCHAR, DOUBLE)
j	JSON

query I
INSERT INTO ch.w_ddl.all_types VALUES (1, true, -1, -2, -3, 4, 5, 6, 7, 8, 9, 1.5, 2.5, 3.1416, 's', 'blob',
    DATE '2024-02-29', TIMESTAMPTZ '2024-02-29 12:34:56.123456+00', TIMESTAMPTZ '2024-02-29 12:34:56.123456+00',
    TIMESTAMPTZ '2024-02-29 12:34:56.123456+00', TIMESTAMPTZ '2024-02-29 12:34:56.123456+00',
    TIMESTAMPTZ '2024-02-29 12:34:56.123456+00', TIME '12:34:56.123456', 'f47ac10b-58cc-4372-a567-0e02b2c3d479', 'y',
    [1, NULL], ROW(1, 'x'), MAP {'k': 1.5}, '{"a": "x"}');
----
1

query IIIIIIIIIIIIIII
SELECT id, b, i8, uh, dec, s, dt, ts_s, ts_ms, ts, t, e, l, st, j FROM ch.w_ddl.all_types;
----
1	true	-1	9	3.1416	s	2024-02-29	2024-02-29 12:34:56+00	2024-02-29 12:34:56.123+00	2024-02-29 12:34:56.123456+00	12:34:56.123456	y	[1, NULL]	{'a': 1, 'b': x}	{"a":"x"}

# ---------------------------------------------------------------------------
# constant DEFAULTs are folded by DuckDB and applied by ClickHouse
# ---------------------------------------------------------------------------
statement ok
CREATE TABLE ch.w_ddl.defaults (id INTEGER PRIMARY KEY, s VARCHAR DEFAULT 'dflt', n BIGINT DEFAULT 40 + 2,
    d DATE DEFAULT DATE '2024-01-01', x DOUBLE DEFAULT -1.5, flag BOOLEAN DEFAULT true);

query I
INSERT INTO ch.w_ddl.defaults (id) VALUES (1);
----
1

query IIIIII
SELECT * FROM ch.w_ddl.defaults;
----
1	dflt	42	2024-01-01	-1.5	true

statement error
CREATE TABLE ch.w_ddl.bad (id INTEGER, t TIMESTAMPTZ DEFAULT now());
----
DEFAULT value of column "t" must be a constant

# ---------------------------------------------------------------------------
# rejected definitions create nothing
# ---------------------------------------------------------------------------
statement error
CREATE TABLE ch.w_ddl.bad (id INTEGER UNIQUE);
----
UNIQUE constraints are not supported for ClickHouse tables

statement error
CREATE TABLE ch.w_ddl.bad (id INTEGER CHECK (id > 0));
----
CHECK constraints are not supported for ClickHouse tables

statement error
CREATE TABLE ch.w_ddl.bad (id INTEGER, id2 INTEGER AS (id + 1));
----
Generated columns are not supported for ClickHouse tables

statement error
CREATE TABLE ch.w_ddl.bad (i INTERVAL);
----
has no ClickHouse equivalent

query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM system.tables WHERE database = ''w_ddl'' AND name = ''bad''');
----
0

# ---------------------------------------------------------------------------
# IF NOT EXISTS, OR REPLACE, duplicates
# ---------------------------------------------------------------------------
statement ok
CREATE TABLE ch.w_ddl.t (id INTEGER);

statement error
CREATE TABLE ch.w_ddl.t (id INTEGER);
----
TABLE_ALREADY_EXISTS

statement ok
CREATE TABLE IF NOT EXISTS ch.w_ddl.t (other VARCHAR);

query I
SELECT column_name FROM information_schema.columns WHERE table_catalog = 'ch' AND table_schema = 'w_ddl' AND table_name = 't';
----
id

statement ok
CREATE OR REPLACE TABLE ch.w_ddl.t (replaced VARCHAR);

query I
SELECT column_name FROM information_schema.columns WHERE table_catalog = 'ch' AND table_schema = 'w_ddl' AND table_name = 't';
----
replaced

query II
SELECT * FROM clickhouse_query('ch', 'SELECT engine, sorting_key FROM system.tables WHERE database = ''w_ddl'' AND name = ''t''');
----
MergeTree	(empty)

# ---------------------------------------------------------------------------
# ch_default_table_engine
# ---------------------------------------------------------------------------
statement error
SET ch_default_table_engine = 'Merge Tree';
----
Invalid ch_default_table_engine

statement error
SET ch_default_table_engine = 'MergeTree() SETTINGS index_granularity = 1';
----
Invalid ch_default_table_engine

statement ok
SET ch_default_table_engine = 'ReplacingMergeTree';

statement ok
CREATE TABLE ch.w_ddl.replacing (id INTEGER PRIMARY KEY, v VARCHAR);

statement ok
SET ch_default_table_engine = 'Memory';

# no ORDER BY for engines outside the MergeTree family
statement ok
CREATE TABLE ch.w_ddl.memory (id INTEGER PRIMARY KEY, v VARCHAR);

statement ok
RESET ch_default_table_engine;

query III
SELECT * FROM clickhouse_query('ch', 'SELECT name, engine, sorting_key FROM system.tables WHERE database = ''w_ddl'' AND name IN (''replacing'', ''memory'') ORDER BY name');
----
memory	Memory	(empty)
replacing	ReplacingMergeTree	id

# ---------------------------------------------------------------------------
# DROP TABLE
# ---------------------------------------------------------------------------
statement ok
DROP TABLE ch.w_ddl.memory;

query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM system.tables WHERE database = ''w_ddl'' AND name = ''memory''');
----
0

statement error
SELECT * FROM ch.w_ddl.memory;
----
does not exist

statement ok
DROP TABLE IF EXISTS ch.w_ddl.memory;

statement error
DROP TABLE ch.w_ddl.memory;
----
does not exist

# ClickHouse views show up as tables, but DROP TABLE does not drop them
statement ok
CALL clickhouse_execute('ch', 'CREATE VIEW w_ddl.v AS SELECT 1 AS one');

statement error
DROP TABLE ch.w_ddl.v;
----
is a ClickHouse view

# DuckDB looks DROP VIEW up among DuckDB views, where ClickHouse views never appear
statement error
DROP VIEW ch.w_ddl.v;

query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM system.tables WHERE database = ''w_ddl'' AND name = ''v''');
----
1

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE w_ddl');
```
`(empty)` is how sqllogictest renders an empty string. If the harness renders it differently, apply the expected-output rule.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/write/ddl_table.test`
Expected: FAIL at the first `CREATE TABLE`, with `CREATE TABLE is not supported on attached ClickHouse databases yet`.

- [ ] **Step 3: Execute with query settings**

In `src/include/clickhouse_connection.hpp`, add next to `Execute(const string &sql)`:
```cpp
	//! Same as Execute(sql), with extra query-level settings. They are sent as non-IMPORTANT settings (a server that
	//! does not know one ignores it), after the connection's own settings
	bool Execute(const string &sql, const vector<std::pair<string, string>> &query_settings);
```
and change the private `MakeQuery` to:
```cpp
	clickhouse::Query MakeQuery(const string &sql,
	                            const vector<std::pair<string, string>> &query_settings = {}) const;
```
In `src/clickhouse_connection.cpp`:
- `MakeQuery` adds, after the user's settings and before `LOW_CARDINALITY_SETTING`:
```cpp
	for (auto &setting : query_settings) {
		query.SetSetting(setting.first, clickhouse::QuerySettingsField {setting.second, 0});
	}
```
- Change `BeginQuery` to take the settings too: `void BeginQuery(const string &sql, const vector<std::pair<string, string>> &query_settings = {})`, passing them to `MakeQuery`. Update the header declaration.
- Make the one-argument `Execute(sql)` call `Execute(sql, {})`, and move its body into the new overload, with `BeginQuery(sql, query_settings)`.

- [ ] **Step 4: Record the table engine**

`src/storage/clickhouse_table_set.cpp`:
- Select `engine` as a third column: `SELECT name, total_rows, engine FROM system.tables WHERE database = …`.
- Keep an `unordered_map<string, string> engines`, and pass `engines[table.name]` to the table entry.

`ClickhouseTableEntry`:
- Add a constructor parameter `string engine` after `approx_rows`.
- Add a member `string engine;  //! system.tables.engine, e.g. MergeTree, View, Dictionary`.
- Add `const string &GetEngine() const { return engine; }`.
- Update the only constructor call, in clickhouse_table_set.cpp.

- [ ] **Step 5: `ClickhouseDdlTypes`**

`src/include/clickhouse_ddl_types.hpp`:
```cpp
#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

//! DuckDB column type -> ClickHouse type, for CREATE TABLE and ALTER TABLE ADD COLUMN: the reverse of
//! ClickhouseTypes::ToDuckDB, chosen so that reading the column back maps to the same (or, for BLOB and the naive
//! TIMESTAMP family, the documented) DuckDB type
class ClickhouseDdlTypes {
public:
	//! `nullable` wraps scalar types in Nullable(). Array, Tuple, Map and JSON cannot be Nullable in ClickHouse and
	//! are never wrapped. Nested element and field types are always nullable (DuckDB LIST/STRUCT/MAP values can hold
	//! NULLs), except Map keys. Throws NotImplementedException for types without a ClickHouse equivalent
	static string ToClickhouse(const LogicalType &type, bool nullable);
};

} // namespace duckdb
```
`src/clickhouse_ddl_types.cpp`:
```cpp
#include "clickhouse_ddl_types.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

static string EnumType(const LogicalType &type) {
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
		entries.push_back(ClickhouseUtils::QuoteLiteral(label_data[i].GetString()) + " = " + to_string(i + 1));
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
		result = EnumType(type);
		break;
	case LogicalTypeId::LIST:
		return "Array(" + ToClickhouse(ListType::GetChildType(type), true) + ")";
	case LogicalTypeId::STRUCT: {
		vector<string> fields;
		for (auto &child : StructType::GetChildTypes(type)) {
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
```
- The `EnumType` helper name shadows DuckDB's `EnumType` struct inside this file. Name it `EnumTypeSql` instead, and keep calling `EnumType::GetSize` / `EnumType::GetValuesInsertOrder` (DuckDB's) in its body.
- If `IsJSONType` / `GetValuesInsertOrder` differ in v1.5.4, look them up in `duckdb/src/include/duckdb/common/types.hpp`.

- [ ] **Step 6: `ClickhouseDdl`: SQL builders, Execute, lookup, CreateTable**

`src/include/storage/clickhouse_ddl.hpp`:
```cpp
#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class ClientContext;
class ClickhouseCatalog;
class ClickhouseTableEntry;
class ColumnDefinition;
struct CreateTableInfo;

//! Builds and runs the ClickHouse DDL for DuckDB DDL statements
class ClickhouseDdl {
public:
	//! CREATE [OR REPLACE] TABLE [IF NOT EXISTS] `database`.`table` (…) ENGINE = <ch_default_table_engine>
	//! [ORDER BY (<primary key>) | ORDER BY tuple()]. Throws NotImplementedException for what ClickHouse tables
	//! cannot express (see the Global Constraints of the Phase 2 plan)
	static string CreateTableSql(ClientContext &context, const string &database, CreateTableInfo &info);
	//! `name` <type> [DEFAULT <constant>]
	static string ColumnSql(ClientContext &context, const ColumnDefinition &column, bool nullable);
	//! " DEFAULT <literal>" for a column with a constant DEFAULT, "" without one
	static string DefaultValueSql(ClientContext &context, const ColumnDefinition &column);
	//! ch_default_table_engine, validated
	static string TableEngine(ClientContext &context);
	//! Throws InvalidInputException unless `engine` is an identifier optionally followed by one (…) group ending
	//! the string
	static void ValidateEngine(const string &engine);
	//! Runs a DDL statement through catalog.StartWrite() (READ_ONLY check, marks the transaction written) with
	//! enable_time_time64_type=1, then clears the catalog's cache -- also when the statement failed. Callers that are
	//! methods of a cached schema entry must hold catalog.GetSchemaEntryOwner(<their name>) across the call
	static void Execute(ClientContext &context, ClickhouseCatalog &catalog, const string &sql);
	//! The (freshly loaded) table entry, or null
	static optional_ptr<ClickhouseTableEntry> LookupTable(ClientContext &context, ClickhouseCatalog &catalog,
	                                                      const string &database, const string &table);
	//! Creates the table (CreateTableSql + Execute) and returns its freshly loaded entry. With IF NOT EXISTS and
	//! an existing table, returns the existing table's entry
	static ClickhouseTableEntry &CreateTable(ClientContext &context, ClickhouseCatalog &catalog,
	                                         const string &database, CreateTableInfo &info);
};

} // namespace duckdb
```
`src/storage/clickhouse_ddl.cpp`:
```cpp
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
		try {
			return ClickhouseFilterPushdown::TransformConstant(value);
		} catch (NotImplementedException &) {
			throw NotImplementedException("DEFAULT value %s of column \"%s\" (type %s) cannot be written as a "
			                              "ClickHouse literal; create the table with clickhouse_execute() instead",
			                              value.ToString(), column_name, value.type().ToString());
		}
	}
}

string ClickhouseDdl::DefaultValueSql(ClientContext &context, const ColumnDefinition &column) {
	if (!column.HasDefaultValue()) {
		return string();
	}
	auto expression = column.DefaultValue().Copy();
	auto binder = Binder::CreateBinder(context);
	ConstantBinder constant_binder(*binder, context, "DEFAULT value");
	auto bound = constant_binder.Bind(expression);
	if (!bound->IsFoldable()) {
		throw NotImplementedException("DEFAULT value of column \"%s\" must be a constant for ClickHouse tables (got "
		                              "%s); create the table with clickhouse_execute() instead",
		                              column.Name(), column.DefaultValue().ToString());
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, *bound).DefaultCastAs(column.Type());
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
		// one parenthesized argument list, closing the string
		valid = engine[position] == '(' && engine.back() == ')';
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

} // namespace duckdb
```
If an include or API differs in v1.5.4, find the real one in `duckdb/src/include` and keep the behaviour. Possible differences:
- `ConstantBinder::Bind` signature;
- `ColumnList::GetColumn(const string &)`;
- `NotNullConstraint::index`;
- `Catalog::GetCatalogTransaction`;
- `EntryLookupInfo`.

`CreateTableInfo` constraints: DuckDB's `Binder::BindNewConstraints` adds NOT NULL constraints for PRIMARY KEY columns into the parsed `info.Base().constraints`, which is what `CreateTableSql` reads. Verify it with the all_types test (`id Int32`).

- [ ] **Step 7: Schema entry hooks**

In `src/storage/clickhouse_schema_entry.cpp`, add the includes `storage/clickhouse_catalog.hpp`, `storage/clickhouse_ddl.hpp`, `storage/clickhouse_table_entry.hpp`, `duckdb/parser/parsed_data/drop_info.hpp`, `duckdb/planner/parsed_data/bound_create_table_info.hpp`, `duckdb/main/client_context.hpp` and `duckdb/common/unordered_set.hpp`, then:
```cpp
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateTable(CatalogTransaction transaction,
                                                              BoundCreateTableInfo &info) {
	auto &context = transaction.GetContext();
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	// ClickhouseDdl clears the catalog cache, which frees cached schema entries -- including this one
	auto keep_alive = ch_catalog.GetSchemaEntryOwner(name);
	return &ClickhouseDdl::CreateTable(context, ch_catalog, name, info.Base());
}
```
```cpp
//! ClickHouse table engines that are not tables: DROP TABLE refuses them
static bool IsViewLikeEngine(const string &engine) {
	return engine == "View" || engine == "MaterializedView" || engine == "LiveView" || engine == "WindowView" ||
	       engine == "Dictionary";
}

void ClickhouseSchemaEntry::DropEntry(ClientContext &context, DropInfo &info) {
	if (info.type != CatalogType::TABLE_ENTRY) {
		throw NotImplementedException("DROP %s is not supported for ClickHouse databases",
		                              CatalogTypeToString(info.type));
	}
	auto entry = tables.GetEntry(context, info.name);
	if (!entry) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Table with name %s does not exist!", info.name);
	}
	auto &table = entry->Cast<ClickhouseTableEntry>();
	if (IsViewLikeEngine(table.GetEngine())) {
		throw NotImplementedException("\"%s\" is a ClickHouse view (engine %s), which DROP TABLE does not drop; "
		                              "drop it with clickhouse_execute('%s', 'DROP VIEW %s.%s') instead",
		                              table.name, table.GetEngine(), catalog.GetName(), name, table.name);
	}
	auto sql = "DROP TABLE " + string(info.if_not_found == OnEntryNotFound::RETURN_NULL ? "IF EXISTS " : "") +
	           ClickhouseUtils::QuoteIdentifier(name) + "." + ClickhouseUtils::QuoteIdentifier(table.name);
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	auto keep_alive = ch_catalog.GetSchemaEntryOwner(name);
	// `table` is freed by the cache clear inside Execute(): not used after this point
	ClickhouseDdl::Execute(context, ch_catalog, sql);
}
```
If `CatalogTypeToString` is named differently, find it in `duckdb/src/include/duckdb/common/enums/catalog_type.hpp`.

- [ ] **Step 8: The setting and the build**

In `src/clickhouse_scanner_extension.cpp`:
```cpp
static void SetClickhouseDefaultTableEngine(ClientContext &context, SetScope scope, Value &parameter) {
	ClickhouseDdl::ValidateEngine(StringValue::Get(parameter));
}
```
```cpp
	config.AddExtensionOption("ch_default_table_engine",
	                          "Table engine for CREATE TABLE in attached ClickHouse databases, e.g. MergeTree or "
	                          "ReplicatedMergeTree('/clickhouse/tables/{shard}/{database}/{table}', '{replica}')",
	                          LogicalType::VARCHAR, Value("MergeTree"), SetClickhouseDefaultTableEngine);
```
Include `storage/clickhouse_ddl.hpp`. Add `clickhouse_ddl_types.cpp` to `src/CMakeLists.txt` and `clickhouse_ddl.cpp` to `src/storage/CMakeLists.txt`, in alphabetical order. Keep the `target_compile_features` lines.

- [ ] **Step 9: README**

Changes:
- **Limitations:** reword the "not supported yet" bullet to: `` `UPDATE` and `DELETE` are not supported yet; run them with `clickhouse_execute()`. ``
- **`## Writing`:** add a DDL paragraph after the INSERT bullets (Task 2 extends it):
```markdown
`CREATE TABLE` (with `IF NOT EXISTS` / `OR REPLACE`) and `DROP TABLE` work on attached databases:

- Column types map back as in the type table, reversed: `VARCHAR`/`BLOB` → `String`, `DATE` → `Date32`,
  `TIMESTAMP`/`TIMESTAMPTZ` → `DateTime64(6, 'UTC')` (`TIMESTAMP_S`/`_MS`/`_NS` → `DateTime('UTC')` /
  `DateTime64(3|9, 'UTC')`), `TIME` → `Time64(6)`, `ENUM` → `Enum8`/`Enum16`, `LIST`/`STRUCT`/`MAP` →
  `Array`/`Tuple`/`Map`, `JSON` → `JSON`. Types with no ClickHouse equivalent (`INTERVAL`, `BIT`, `UNION`, …) are
  rejected. Naive `TIMESTAMP` columns are stored as UTC and read back as `TIMESTAMP WITH TIME ZONE`, and `BLOB` reads
  back as `VARCHAR`.
- Nullable columns become `Nullable(T)`; `PRIMARY KEY` and `NOT NULL` columns do not. `Array`, `Tuple`, `Map` and
  `JSON` columns cannot be `Nullable` in ClickHouse, so they cannot hold `NULL`.
- The engine is `ch_default_table_engine` (default `MergeTree`, e.g. `SET ch_default_table_engine =
  'ReplicatedMergeTree'` on a self-hosted cluster). MergeTree-family tables are `ORDER BY` the `PRIMARY KEY`, or
  `tuple()` without one.
- `DEFAULT` values must be constants. `UNIQUE`, `CHECK` and `FOREIGN KEY` constraints, generated columns and
  `PARTITIONED BY` / `SORTED BY` are not supported; use `clickhouse_execute` for those, and for `CREATE VIEW`.
- `DROP TABLE` does not drop ClickHouse views or dictionaries, and `DROP VIEW` cannot reach them (DuckDB only looks
  for DuckDB views); drop them with `clickhouse_execute`.
```
Add a Settings table row after `ch_insert_block_size`:
```markdown
| `ch_default_table_engine` | `MergeTree` | Table engine used by `CREATE TABLE` |
```

- [ ] **Step 10: Run the tests to verify they pass**

Run: `make release && make smoke`. Then run `make debug && make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*'`, and the no-server suite. Expected: all pass.

- [ ] **Step 11: Commit**

```bash
git add -A src test README.md
git commit -m "feat: CREATE TABLE and DROP TABLE on attached ClickHouse databases

<your harness's Co-Authored-By trailer>"
```

---

### Task 2: CREATE / DROP SCHEMA and ALTER TABLE

**Files:**
- Modify: `src/storage/clickhouse_catalog.cpp` (CreateSchema, DropSchema)
- Modify: `src/storage/clickhouse_schema_entry.cpp` (Alter)
- Modify: `src/include/storage/clickhouse_ddl.hpp`, `src/storage/clickhouse_ddl.cpp` (AlterTableSql)
- Modify: `test/sql/write/unsupported.test`, `test/sql/attach/attach.test`
- Create: `test/sql/write/ddl_schema_alter.test`
- Modify: `README.md`

**Interfaces:**
- Consumes (Task 1):
  - `ClickhouseDdl::Execute`, `ClickhouseDdl::ColumnSql`, `ClickhouseDdl::LookupTable`
  - `ClickhouseTableEntry::GetEngine`
  - `ClickhouseCatalog::GetSchemaEntryOwner`
- Produces: `static string ClickhouseDdl::AlterTableSql(ClientContext &, const string &database, const string &table, AlterTableInfo &)`

- [ ] **Step 1: Write the failing test**

Fixture edits:
- In `test/sql/write/unsupported.test`, **replace** the `ALTER TABLE ch.test_db.t1 ADD COLUMN extra INTEGER;` block, which would now modify the fixture, with:
```
statement error
ALTER TABLE ch.test_db.t1 ALTER COLUMN name TYPE INTEGER;
----
is not supported for ClickHouse tables
```
- In `test/sql/write/unsupported.test`, **delete** the `CREATE SCHEMA ch.new_db;` block.
- In `test/sql/attach/attach.test`, **delete** the `CREATE SCHEMA ch.new_db;` statement-error block.

`test/sql/write/ddl_schema_alter.test`:
```
# name: test/sql/write/ddl_schema_alter.test
# description: CREATE/DROP SCHEMA and ALTER TABLE on an attached ClickHouse database
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE IF EXISTS w_ddl2');

# ---------------------------------------------------------------------------
# CREATE SCHEMA
# ---------------------------------------------------------------------------
statement ok
CREATE SCHEMA ch.w_ddl2;

query I
SELECT count(*) FROM duckdb_schemas() WHERE database_name = 'ch' AND schema_name = 'w_ddl2';
----
1

query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM system.databases WHERE name = ''w_ddl2''');
----
1

statement error
CREATE SCHEMA ch.w_ddl2;
----
DATABASE_ALREADY_EXISTS

statement ok
CREATE SCHEMA IF NOT EXISTS ch.w_ddl2;

# ---------------------------------------------------------------------------
# ALTER TABLE
# ---------------------------------------------------------------------------
statement ok
CREATE TABLE ch.w_ddl2.t (id INTEGER PRIMARY KEY, a VARCHAR);

statement ok
INSERT INTO ch.w_ddl2.t VALUES (1, 'x');

statement ok
ALTER TABLE ch.w_ddl2.t ADD COLUMN c INTEGER DEFAULT 7;

query III
SELECT * FROM ch.w_ddl2.t;
----
1	x	7

query III
SELECT * FROM clickhouse_query('ch', 'SELECT name, type, default_expression FROM system.columns WHERE database = ''w_ddl2'' AND table = ''t'' AND name = ''c''');
----
c	Nullable(Int32)	7

statement error
ALTER TABLE ch.w_ddl2.t ADD COLUMN c INTEGER;
----
ClickHouse error

statement ok
ALTER TABLE ch.w_ddl2.t ADD COLUMN IF NOT EXISTS c INTEGER;

statement ok
ALTER TABLE ch.w_ddl2.t RENAME COLUMN a TO b;

query I
SELECT b FROM ch.w_ddl2.t;
----
x

statement ok
ALTER TABLE ch.w_ddl2.t DROP COLUMN c;

query I
SELECT count(*) FROM information_schema.columns WHERE table_catalog = 'ch' AND table_schema = 'w_ddl2' AND table_name = 't';
----
2

statement ok
ALTER TABLE ch.w_ddl2.t DROP COLUMN IF EXISTS c;

statement ok
ALTER TABLE ch.w_ddl2.t RENAME TO u;

query II
SELECT * FROM ch.w_ddl2.u;
----
1	x

statement error
SELECT * FROM ch.w_ddl2.t;
----
does not exist

query I
SELECT * FROM clickhouse_query('ch', 'SELECT name FROM system.tables WHERE database = ''w_ddl2''');
----
u

statement error
ALTER TABLE ch.w_ddl2.u ALTER COLUMN b TYPE INTEGER;
----
is not supported for ClickHouse tables

statement error
ALTER TABLE ch.w_ddl2.u ALTER COLUMN b SET DEFAULT 'z';
----
is not supported for ClickHouse tables

# ---------------------------------------------------------------------------
# DROP SCHEMA
# ---------------------------------------------------------------------------
statement error
DROP SCHEMA ch.w_ddl2;
----
use DROP SCHEMA … CASCADE

statement ok
DROP SCHEMA ch.w_ddl2 CASCADE;

query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM system.databases WHERE name = ''w_ddl2''');
----
0

query I
SELECT count(*) FROM duckdb_schemas() WHERE database_name = 'ch' AND schema_name = 'w_ddl2';
----
0

statement ok
DROP SCHEMA IF EXISTS ch.w_ddl2;

statement error
DROP SCHEMA ch.w_ddl2;
----
does not exist

# an empty schema drops without CASCADE
statement ok
CREATE SCHEMA ch.w_ddl2;

statement ok
DROP SCHEMA ch.w_ddl2;

query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM system.databases WHERE name = ''w_ddl2''');
----
0
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/write/ddl_schema_alter.test`
Expected: FAIL at `CREATE SCHEMA ch.w_ddl2` with `CREATE SCHEMA is not supported on attached ClickHouse databases yet`.

- [ ] **Step 3: CREATE / DROP SCHEMA**

In `src/storage/clickhouse_catalog.cpp` (include `storage/clickhouse_ddl.hpp` and `duckdb/parser/parsed_data/drop_info.hpp`):
```cpp
optional_ptr<CatalogEntry> ClickhouseCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	auto &context = transaction.GetContext();
	string sql = "CREATE DATABASE ";
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
		sql += "IF NOT EXISTS ";
	} else if (info.on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
		throw NotImplementedException("CREATE OR REPLACE SCHEMA is not supported for ClickHouse databases");
	}
	ClickhouseDdl::Execute(context, *this, sql + ClickhouseUtils::QuoteIdentifier(info.schema));
	return LookupSchema(transaction, EntryLookupInfo(CatalogType::SCHEMA_ENTRY, info.schema),
	                    OnEntryNotFound::RETURN_NULL);
}

void ClickhouseCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	auto transaction = GetCatalogTransaction(context);
	auto schema = LookupSchema(transaction, EntryLookupInfo(CatalogType::SCHEMA_ENTRY, info.name),
	                           OnEntryNotFound::RETURN_NULL);
	if (!schema) {
		if (info.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Schema with name \"%s\" does not exist!", info.name);
	}
	auto database = schema->name;
	if (!info.cascade) {
		// DROP DATABASE drops everything in it: match DuckDB, which refuses to drop a non-empty schema without CASCADE
		idx_t table_count = 0;
		{
			auto connection = connection_pool->GetConnection();
			for (auto &block : connection->Query("SELECT count() FROM system.tables WHERE database = " +
			                                     ClickhouseUtils::QuoteLiteral(database))) {
				if (block.GetRowCount() > 0) {
					table_count = block[0]->As<clickhouse::ColumnUInt64>()->At(0);
				}
			}
		}
		if (table_count > 0) {
			throw CatalogException("Cannot drop ClickHouse database \"%s\": it contains %d table(s); use DROP SCHEMA "
			                       "… CASCADE to drop it with everything in it",
			                       database, table_count);
		}
	}
	ClickhouseDdl::Execute(context, *this,
	                       "DROP DATABASE " +
	                           string(info.if_not_found == OnEntryNotFound::RETURN_NULL ? "IF EXISTS " : "") +
	                           ClickhouseUtils::QuoteIdentifier(database));
}
```
Name clash: `CreateSchemaInfo` stores the schema name in `info.schema` (CreateInfo::schema). Verify this in v1.5.4.

- [ ] **Step 4: ALTER TABLE**

Add to `ClickhouseDdl`, and include `duckdb/parser/parsed_data/alter_table_info.hpp` in the .cpp:
```cpp
	//! ALTER TABLE ADD COLUMN [IF NOT EXISTS] / DROP COLUMN [IF EXISTS] / RENAME COLUMN, or RENAME TABLE; anything
	//! else throws NotImplementedException
	static string AlterTableSql(ClientContext &context, const string &database, const string &table,
	                            AlterTableInfo &info);
```
```cpp
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
```
In `src/storage/clickhouse_schema_entry.cpp`:
```cpp
void ClickhouseSchemaEntry::Alter(CatalogTransaction transaction, AlterInfo &info) {
	if (info.type != AlterType::ALTER_TABLE) {
		throw NotImplementedException("This ALTER statement is not supported for ClickHouse databases; run it with "
		                              "clickhouse_execute() instead");
	}
	auto &context = transaction.GetContext();
	auto &alter = info.Cast<AlterTableInfo>();
	auto entry = tables.GetEntry(context, alter.name);
	if (!entry) {
		if (alter.if_not_found == OnEntryNotFound::RETURN_NULL) {
			return;
		}
		throw CatalogException("Table with name %s does not exist!", alter.name);
	}
	// build the SQL before Execute() clears the cache and frees `entry`
	auto sql = ClickhouseDdl::AlterTableSql(context, name, entry->name, alter);
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	auto keep_alive = ch_catalog.GetSchemaEntryOwner(name);
	ClickhouseDdl::Execute(context, ch_catalog, sql);
}
```
Include `duckdb/parser/parsed_data/alter_table_info.hpp`. If DuckDB's binder rejects one of the tested ALTER forms before our hook runs, e.g. `ALTER COLUMN … TYPE` on a non-DuckDB table, accept DuckDB's own message in the test and note it.

- [ ] **Step 5: README**

In `## Writing`, extend the DDL paragraph's first line to: `` `CREATE TABLE` (with `IF NOT EXISTS` / `OR REPLACE`), `DROP TABLE`, `CREATE SCHEMA` / `DROP SCHEMA` (ClickHouse databases; a non-empty one needs `CASCADE`) and `ALTER TABLE … ADD COLUMN` / `DROP COLUMN` / `RENAME COLUMN` / `RENAME TO` work on attached databases: ``. Add this bullet:
```markdown
- Other `ALTER TABLE` forms (changing a column's type or default, constraints) are not supported; use
  `clickhouse_execute`. Columns added with `ALTER TABLE … ADD COLUMN` are always `Nullable`.
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `make release && make smoke`, then `make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*'` on a debug build. Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add -A src test README.md
git commit -m "feat: CREATE/DROP SCHEMA and ALTER TABLE on attached ClickHouse databases

<your harness's Co-Authored-By trailer>"
```

---

### Task 3: CREATE TABLE … AS SELECT

**Files:**
- Modify: `src/include/storage/clickhouse_insert.hpp`, `src/storage/clickhouse_insert.cpp`
- Modify: `src/storage/clickhouse_catalog.cpp` (PlanCreateTableAs)
- Modify: `src/clickhouse_writer.cpp` (naive TIMESTAMP family sources)
- Modify: `test/sql/write/unsupported.test`, `test/sql/write/read_only_attach.test`
- Create: `test/sql/write/ctas.test`
- Modify: `README.md`

**Interfaces:**
- Consumes:
  - Task 1: `ClickhouseDdl::CreateTableSql`, `ClickhouseDdl::CreateTable`, `ClickhouseDdl::LookupTable`, `ClickhouseDdlTypes::ToClickhouse`
  - Phase 1: `ClickhouseInsert` (catalog_name, database_name, table_name, columns, insert_sql, `StartInsert`, `GetInsertColumns`, `BuildInsertQuery`), `ClickhouseWriter::GetWriteMode`, `ClickhouseTypeParser::Parse`
- Produces:
  - `ClickhouseInsert(PhysicalPlan &, LogicalOperator &, ClickhouseCatalog &catalog, const string &database, unique_ptr<BoundCreateTableInfo> create_info)`, the CTAS constructor
  - `ClickhouseInsertGlobalState::columns` / `insert_sql`: the resolved target

- [ ] **Step 1: Write the failing test**

Fixture edits:
- In `test/sql/write/unsupported.test`, **delete** the `CREATE TABLE ch.test_db.new_table AS SELECT 42 AS i;` block.
- In `test/sql/write/read_only_attach.test`, add before the final count check:
```
statement error
CREATE TABLE ch.test_db.ctas AS SELECT 42 AS i;
----
attached in read-only mode
```

`test/sql/write/ctas.test`:
```
# name: test/sql/write/ctas.test
# description: CREATE TABLE … AS SELECT into an attached ClickHouse database
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

require icu

statement ok
SET TimeZone = 'UTC';

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE IF EXISTS w_ctas');

statement ok
CALL clickhouse_execute('ch', 'CREATE DATABASE w_ctas');

# many blocks, nested and naive-timestamp columns
statement ok
SET ch_insert_block_size = 1000;

query I
CREATE TABLE ch.w_ctas.t1 AS
SELECT range::INTEGER AS id, 'v' || range AS s, (range * 1.5)::DOUBLE AS d,
       TIMESTAMP '2024-01-01 00:00:00' + INTERVAL (range) HOUR AS ts, [range, NULL] AS l
FROM range(5000);
----
5000

statement ok
RESET ch_insert_block_size;

query II
SELECT * FROM clickhouse_query('ch', 'SELECT name, type FROM system.columns WHERE database = ''w_ctas'' AND table = ''t1'' ORDER BY position');
----
id	Nullable(Int32)
s	Nullable(String)
d	Nullable(Float64)
ts	Nullable(DateTime64(6, 'UTC'))
l	Array(Nullable(Int64))

query IIII
SELECT count(*), sum(id), max(ts), max(len(l)) FROM ch.w_ctas.t1;
----
5000	12497500	2024-07-27 07:00:00+00	2

query I
SELECT * FROM clickhouse_query('ch', 'SELECT toString(max(ts)) FROM w_ctas.t1');
----
2024-07-27 07:00:00.000000

# an empty result still creates the table
query I
CREATE TABLE ch.w_ctas.empty AS SELECT 1::INTEGER AS x WHERE false;
----
0

query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM system.columns WHERE database = ''w_ctas'' AND table = ''empty''');
----
1

# IF NOT EXISTS leaves an existing table alone and writes nothing
query I
CREATE TABLE IF NOT EXISTS ch.w_ctas.t1 AS SELECT 1 AS other;
----
0

query I
SELECT count(*) FROM ch.w_ctas.t1;
----
5000

# OR REPLACE replaces it
query I
CREATE OR REPLACE TABLE ch.w_ctas.t1 AS SELECT 42::INTEGER AS answer;
----
1

query I
SELECT * FROM ch.w_ctas.t1;
----
42

# naive timestamp family, BLOB and ENUM sources keep their values
query I
CREATE TABLE ch.w_ctas.types AS SELECT
    TIMESTAMP_S '2024-01-01 00:00:01' AS ts_s, TIMESTAMP_MS '2024-01-01 00:00:00.123' AS ts_ms,
    TIMESTAMP_NS '2024-01-01 00:00:00.123456789' AS ts_ns, 'abc'::BLOB AS bl, 'y'::ENUM('x', 'y') AS e;
----
1

query IIIII
SELECT * FROM clickhouse_query('ch', 'SELECT toString(ts_s), toString(ts_ms), toString(ts_ns), hex(bl), toString(e) FROM w_ctas.types');
----
2024-01-01 00:00:01	2024-01-01 00:00:00.123	2024-01-01 00:00:00.123456789	616263	y

# from a DuckDB table
statement ok
CREATE TABLE local_rows AS SELECT range AS n FROM range(3);

query I
CREATE TABLE ch.w_ctas.from_local AS SELECT * FROM local_rows;
----
3

query I
SELECT sum(n) FROM ch.w_ctas.from_local;
----
3

# types ClickHouse cannot hold fail before anything is created
statement error
CREATE TABLE ch.w_ctas.bad AS SELECT INTERVAL 1 DAY AS i;
----
has no ClickHouse equivalent

query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM system.tables WHERE database = ''w_ctas'' AND name = ''bad''');
----
0

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE w_ctas');
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/write/ctas.test`
Expected: FAIL at the first CTAS with `CREATE TABLE AS is not supported on attached ClickHouse databases yet`.

- [ ] **Step 3: Accept naive TIMESTAMP family sources in the writer**

In `src/clickhouse_writer.cpp`, replace `AppendTimestamp` so that it accepts TIMESTAMP_TZ and also TIMESTAMP, TIMESTAMP_SEC, TIMESTAMP_MS and TIMESTAMP_NS. All of them are int64 tick counts at precisions 6, 6, 0, 3 and 9. Naive timestamps are written as UTC.
- A regular INSERT always sends TIMESTAMP_TZ, because the read mapping types those columns so.
- A CTAS sends the query's own types.

```cpp
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
```
Keep the existing range-error message wording ("outside the range of ClickHouse type …"); the Phase 1 tests assert it. Some changes to the value text in messages may be needed. Check `test/sql/write/insert.test`, which only asserts "outside the range of ClickHouse type".

- [ ] **Step 4: CTAS mode in `ClickhouseInsert`**

Header (`src/include/storage/clickhouse_insert.hpp`):
- Forward-declare `class ClickhouseCatalog; struct BoundCreateTableInfo;`.
- Add the constructor:
```cpp
	//! CREATE TABLE … AS SELECT: the table is created when the statement runs (on the first row, or in Finalize
	//! when the query yields none), then filled like an INSERT listing every column
	ClickhouseInsert(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseCatalog &catalog,
	                 const string &database, unique_ptr<BoundCreateTableInfo> create_info);

	//! CTAS only: the table to create
	unique_ptr<BoundCreateTableInfo> create_info;
```
- Document that `columns` and `insert_sql` are the INSERT target resolved at plan time, and are empty for CTAS.

In `src/storage/clickhouse_insert.cpp`:
- Add `vector<ClickhouseInsertColumn> columns; string insert_sql; bool target_ready = false; bool skip = false;` to `ClickhouseInsertGlobalState`. Their comments: "the target: copied from the operator for INSERT, resolved after creating the table for CTAS"; "CTAS with IF NOT EXISTS on an existing table: write nothing".
- Add a method that resolves the target without taking the insert connection:
```cpp
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
	auto &info = op.create_info->Base();
	if (info.on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT &&
	    ClickhouseDdl::LookupTable(context, catalog, op.database_name, info.table)) {
		gstate.skip = true;
		return;
	}
	auto &table = ClickhouseDdl::CreateTable(context, catalog, op.database_name, info);
	// every column, in query order: the CTAS input chunk has one column per created column
	gstate.columns = ClickhouseInsert::GetInsertColumns(table, physical_index_vector_t<idx_t>());
	gstate.insert_sql = ClickhouseInsert::BuildInsertQuery(table, gstate.columns);
}
```
- `StartInsert` calls `PrepareTarget(*this, context, gstate)` first. It then uses `gstate.insert_sql` / `gstate.columns` instead of the members, including the header column-count check.
- `Sink` returns `SinkResultType::FINISHED` when `gstate.skip` is set, after `PrepareTarget`, and otherwise loops over `gstate.columns`. Structure: if `!gstate.target_ready`, call PrepareTarget; if skip, return FINISHED; if there is no connection, call StartInsert.
- `Finalize`, before the "no connection" early return: `if (create_info) { PrepareTarget(*this, context, gstate); }`. An empty CTAS still creates the table.
- The CTAS constructor sets `catalog_name = catalog.GetName()`, `database_name = database`, `table_name = create_info->Base().table`, and moves `create_info` in.
- `GetName()` returns `create_info ? "CLICKHOUSE_CREATE_TABLE_AS" : "CLICKHOUSE_INSERT"`.
- Include `storage/clickhouse_ddl.hpp` and `duckdb/planner/parsed_data/bound_create_table_info.hpp`.

- [ ] **Step 5: `PlanCreateTableAs`**

In `src/storage/clickhouse_catalog.cpp` (include `duckdb/planner/operator/logical_create_table.hpp`, `duckdb/planner/parsed_data/bound_create_table_info.hpp`, `clickhouse_ddl_types.hpp` and `clickhouse_writer.hpp`):
```cpp
PhysicalOperator &ClickhouseCatalog::PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner,
                                                       LogicalCreateTable &op, PhysicalOperator &plan) {
	ThrowIfReadOnly();
	auto &info = op.info->Base();
	// fail now, before anything is created, for columns ClickHouse cannot hold or that the INSERT cannot write
	ClickhouseDdl::CreateTableSql(context, op.schema.name, info);
	for (auto &column : info.columns.Logical()) {
		auto type = ClickhouseDdlTypes::ToClickhouse(column.Type(), true);
		if (ClickhouseWriter::GetWriteMode(ClickhouseTypeParser::Parse(type)) == ClickhouseWriteMode::UNSUPPORTED) {
			throw NotImplementedException("Column \"%s\" (%s) cannot be written into ClickHouse by CREATE TABLE AS; "
			                              "create the table with clickhouse_execute() instead",
			                              column.Name(), column.Type().ToString());
		}
	}
	auto &insert = planner.Make<ClickhouseInsert>(op, *this, op.schema.name, std::move(op.info));
	insert.children.push_back(plan);
	return insert;
}
```

- [ ] **Step 6: README**

In `## Writing`, extend the DDL paragraph with this bullet:
```markdown
- `CREATE TABLE … AS SELECT` creates the table (all columns `Nullable`, `ORDER BY tuple()` for MergeTree engines),
  then streams the rows in like an `INSERT`. It is not atomic: if the `INSERT` part fails, the table stays, possibly
  with some rows.
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `make release && make smoke`, then `make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*'` after `make debug`, and the no-server suite. Expected: all pass.

- [ ] **Step 8: Commit**

```bash
git add -A src test README.md
git commit -m "feat: CREATE TABLE … AS SELECT into attached ClickHouse databases

<your harness's Co-Authored-By trailer>"
```

---

## Spec coverage (Phase 2, spec §10.2)

| Spec item | Task |
|---|---|
| Reverse type mapping and nullability rules (§5) | 1 |
| CREATE TABLE with IF NOT EXISTS / OR REPLACE, engine, ORDER BY PK or tuple() (§5, D4) | 1 |
| `ch_default_table_engine` and its validation (§2) | 1 |
| Rejected constraints (§2, §5) | 1 |
| Engine-aware DROP (§5) | 1: DROP TABLE refuses views; DROP VIEW is unreachable, a recorded deviation |
| CREATE / DROP SCHEMA with a CASCADE check (§5) | 2 |
| ALTER add, drop and rename column; rename table; others rejected (§5) | 2 |
| CTAS (§4) | 3 |
| Record the engine in the table entry (§3) | 1 |
| Cache invalidation after DDL (§3) | 1–3, through `ClickhouseDdl::Execute` |
| Tests checked through DuckDB and ClickHouse, debug build (§9) | 1–3 |

Deviations from the spec, each recorded as a ruling:
- **DEFAULT must be a constant.** Spec §5 says DEFAULT goes through `clickhouse_expression`, which is Phase 3's translator for *bound* WHERE/SET expressions. DEFAULTs are parsed expressions: DuckDB folds constants, and anything else points to `clickhouse_execute()`.
- **DROP VIEW is not supported,** for the type-confusion reason given under Global Constraints.
