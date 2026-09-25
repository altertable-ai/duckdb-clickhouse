# ClickHouse writes — Phase 1 (write foundation, INSERT/COPY, clickhouse_execute) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make attached ClickHouse databases writable. After this plan: `READ_ONLY` becomes opt-in; `INSERT` / `INSERT … SELECT` / `COPY … FROM` stream native blocks into ClickHouse; `clickhouse_execute()` runs arbitrary ClickHouse SQL; and ROLLBACK after a ClickHouse write logs a warning.

**Architecture:**
- Write statements now reach the extension, because only a `READ_ONLY` ATTACH makes DuckDB reject them.
- `ClickhouseCatalog::PlanInsert` returns a single-threaded `ClickhouseInsert` sink. The sink opens one pooled connection, runs `INSERT INTO db.t (cols) VALUES` through a patched clickhouse-cpp `BeginInsert(const Query&)` (so settings travel with it), converts DuckDB chunks into clickhouse-cpp columns with `ClickhouseWriter`, and sends a block every `ch_insert_block_size` rows.
- Column types that the read path converts on the server (IPv4/IPv6, Int256, Decimal256, JSON, geo) are written as strings through `INSERT … SELECT … FROM input(…)`.
- DDL, UPDATE and DELETE are Phases 2 and 3. Until then they fail with a clear "not supported yet; use clickhouse_execute()" error.

**Tech Stack:**
- C++17, DuckDB v1.5.4 (submodule `duckdb` at 08e34c4)
- clickhouse-cpp 2.6.2 via the vcpkg overlay port (plus one new patch)
- sqllogictest via `make smoke` (a throw-away ClickHouse 25.8 container)

**Spec:** `docs/superpowers/specs/2026-09-24-clickhouse-writes-design.md`. This plan implements its Phase 1 (§10.1). Also read §2, §4, §7, §8 and §9.

## Global Constraints

- **Branch:** work on `su/init` (checked out). Commit there; never push.
- **Writable by default.** `ATTACH … (TYPE clickhouse, READ_ONLY)` is read-only and DuckDB rejects writes to it ("attached in read-only mode").
- **Transactions (D5):** every write is committed immediately. COMMIT is a no-op. ROLLBACK cannot undo ClickHouse writes and must emit `DUCKDB_LOG_WARNING` with the text `ClickHouse writes made in this transaction were already committed and cannot be rolled back (database "<name>")`.
- **INSERT sends only the target columns** (`INSERT INTO db.t (cols)`); ClickHouse applies its own DEFAULT/MATERIALIZED values. MATERIALIZED and ALIAS columns cannot be INSERT targets. Values ClickHouse cannot store raise `ConversionException` naming the column and are never clamped.
- **ClickHouse ranges** (inclusive):
  - Date 1970-01-01..2149-06-06
  - Date32 1900-01-01..2299-12-31
  - DateTime 1970-01-01 00:00:00..2106-02-07 06:28:15
  - DateTime64 1900-01-01 00:00:00..2299-12-31 23:59:59, and additionally limited by int64 ticks at precision > 6
- **New settings:** `ch_insert_block_size` (UBIGINT, default 65536, must be > 0).
- **New SQL function:** `clickhouse_execute(database VARCHAR, sql VARCHAR) → (Success BOOLEAN)`. It clears that catalog's cache and rejects statements that return rows (use `clickhouse_query`).
- **Unsupported statements:** DDL/UPDATE/DELETE/CTAS on a writable attach throw `NotImplementedException("<STATEMENT> is not supported on attached ClickHouse databases yet; run it in ClickHouse with clickhouse_execute() instead")`. `MERGE INTO`, `CREATE INDEX` and `CREATE VIEW` are permanently unsupported, with their own messages.
- **Passwords** never appear in errors, logs or `duckdb_databases()`.
- **Every src/ object library** keeps `target_compile_features(<lib> PUBLIC cxx_std_17)`.
- **Tests:**
  - Server tests start with `require-env CLICKHOUSE_TEST_HOST` (+ `_PORT`, `_USER`, `_PASSWORD`) and attach with `host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD}`.
  - Write tests create and drop their own ClickHouse database (`w_*`) through `clickhouse_execute`; they never modify the `test_db` / `other_db` fixtures.
  - Every write is checked both through DuckDB and through `clickhouse_query` (ClickHouse's own view).
- **Commit trailer:** use the Co-Authored-By trailer your own harness instructs you to use.

## Build & test cheat sheet

```bash
cd /Users/redox/dev/altertable-ai/duckdb-clickhouse
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja
make release                                   # incremental; Task 2's port patch rebuilds clickhouse-cpp once
./build/release/test/unittest "test/*"         # no-server tests (server tests skip)
make smoke                                     # all tests against a throw-away ClickHouse 25.8 (Docker)
make smoke ARGS=test/sql/write/insert.test     # one file; ARGS='test/sql/write/*' for a glob
make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*'   # debug build (make debug first)
CLICKHOUSE_TEST_KEEP=1 make smoke ARGS=...     # keep the container; inspect with
docker exec -it clickhouse-scanner-test clickhouse-client --user duckdb --password duckdb   # then docker rm -f clickhouse-scanner-test
```

- Long builds or test runs: run them in the background and poll every few minutes, printing progress. An agent that is idle for 10 minutes is killed.
- Check Docker with `timeout 20 docker info`. Never restart Docker Desktop.

**Expected-output rule for tests:** expectations are DuckDB sqllogictest renderings. If a value differs only in formatting, fix the expectation. If the value itself is different, fix the code. List every such change in your report.

## File Structure

```
vcpkg_ports/clickhouse-cpp/insert-query-settings.patch   NEW  BeginInsert(const Query&) sends query settings
vcpkg_ports/clickhouse-cpp/portfile.cmake, vcpkg.json    MOD  apply the patch, bump port-version
src/include/clickhouse_utils.hpp, src/clickhouse_utils.cpp            MOD  ThrowUnsupportedWrite, ThrowReadOnly(db), StripTrailingSemicolons
src/include/clickhouse_connection.hpp, src/clickhouse_connection.cpp  MOD  Execute, BeginInsert/SendInsertBlock/EndInsert/AbortInsert
src/include/clickhouse_execute.hpp, src/clickhouse_execute.cpp        NEW  clickhouse_execute() table function
src/include/clickhouse_writer.hpp, src/clickhouse_writer.cpp          NEW  DuckDB Vector → clickhouse-cpp column appends
src/include/storage/clickhouse_insert.hpp, src/storage/clickhouse_insert.cpp  NEW  ClickhouseInsert sink operator
src/include/storage/clickhouse_transaction.hpp, src/storage/clickhouse_transaction.cpp  MOD  write tracking + ROLLBACK warning
src/storage/clickhouse_catalog.cpp, src/storage/clickhouse_schema_entry.cpp   MOD  writable semantics, PlanInsert
src/storage/clickhouse_connection_pool.cpp                MOD  never recover a connection left mid-insert
src/include/clickhouse_types.hpp, src/storage/clickhouse_table_set.cpp  MOD  ClickhouseColumnInfo::default_kind
src/clickhouse_conversion.cpp                             MOD  PowerOfTen/ScaleTicks move to ClickhouseUtils
src/clickhouse_query.cpp                                  MOD  use ClickhouseUtils::StripTrailingSemicolons
src/clickhouse_scanner_extension.cpp                      MOD  register clickhouse_execute, ch_insert_block_size
src/CMakeLists.txt, src/storage/CMakeLists.txt            MOD  new sources
test/sql/write/read_only_attach.test, unsupported.test, execute.test, insert.test, insert_converted.test   NEW
test/sql/scan/read_only.test, test/sql/catalog/read_only.test   DELETE (replaced by test/sql/write/unsupported.test)
test/sql/attach/attach.test                               MOD  CREATE SCHEMA expectation
README.md                                                 MOD  writable default, READ_ONLY, transactions, INSERT, clickhouse_execute
```

## Task Order

1. Writable-by-default semantics, transaction write tracking and the ROLLBACK warning mechanism.
2. The clickhouse-cpp insert patch, the connection write API and `clickhouse_execute()`.
3. `ClickhouseWriter` + `ClickhouseInsert`: INSERT / INSERT … SELECT / COPY for natively-encoded types.
4. Server-converted column types through `input()`.

Each task depends on the ones before it.

---

### Task 1: Writable-by-default semantics and transaction write tracking

**Files:**
- Modify: `src/include/clickhouse_utils.hpp`, `src/clickhouse_utils.cpp`
- Modify: `src/storage/clickhouse_catalog.cpp`, `src/storage/clickhouse_schema_entry.cpp`
- Modify: `src/include/storage/clickhouse_transaction.hpp`, `src/storage/clickhouse_transaction.cpp`
- Modify: `test/sql/attach/attach.test`, `README.md`
- Create: `test/sql/write/read_only_attach.test`, `test/sql/write/unsupported.test`
- Delete: `test/sql/scan/read_only.test`, `test/sql/catalog/read_only.test`

**Interfaces:**
- Produces:
  - `[[noreturn]] ClickhouseUtils::ThrowUnsupportedWrite(const string &statement)`
  - `[[noreturn]] ClickhouseUtils::ThrowReadOnly(const string &database_name)`: replaces the old zero-argument `ThrowReadOnly()`.
  - `ClickhouseTransaction::MarkWritten()`, `bool ClickhouseTransaction::HasWritten() const`
  - `static ClickhouseTransaction &ClickhouseTransaction::Get(ClientContext &context, Catalog &catalog)`
  - The ROLLBACK warning, emitted from `ClickhouseTransactionManager::RollbackTransaction`.

- [ ] **Step 1: Write the failing tests**

Delete `test/sql/scan/read_only.test` and `test/sql/catalog/read_only.test` (`git rm`). Their cases move to the two files below.

`test/sql/write/read_only_attach.test`:
```
# name: test/sql/write/read_only_attach.test
# description: an ATTACH with READ_ONLY rejects every write; reads keep working
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse, READ_ONLY);

query I
SELECT count(*) FROM ch.test_db.t1;
----
3

statement error
INSERT INTO ch.test_db.t1 (id, name) VALUES (4, 'Dan');
----
attached in read-only mode

statement error
UPDATE ch.test_db.t1 SET name = 'x';
----
attached in read-only mode

statement error
DELETE FROM ch.test_db.t1;
----
attached in read-only mode

statement error
CREATE TABLE ch.test_db.new_table (i INTEGER);
----
attached in read-only mode

statement error
DROP TABLE ch.test_db.t1;
----
attached in read-only mode

statement error
CREATE SCHEMA ch.new_db;
----
attached in read-only mode

statement error
MERGE INTO ch.test_db.t1 USING (VALUES (1)) src(id) ON t1.id = src.id
WHEN MATCHED THEN UPDATE SET name = 'x';
----
attached in read-only mode

# indexes are rejected while binding, before DuckDB's read-only check runs
statement error
CREATE INDEX idx ON ch.test_db.t1(id);
----
Indexes are not supported for ClickHouse tables

query I
SELECT count(*) FROM ch.test_db.t1;
----
3
```

`test/sql/write/unsupported.test`:
```
# name: test/sql/write/unsupported.test
# description: on a writable attach, statements that are not implemented yet fail clearly and change nothing
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

statement error
UPDATE ch.test_db.t1 SET name = 'x';
----
UPDATE is not supported on attached ClickHouse databases yet

statement error
DELETE FROM ch.test_db.t1;
----
DELETE is not supported on attached ClickHouse databases yet

statement error
CREATE TABLE ch.test_db.new_table (i INTEGER);
----
CREATE TABLE is not supported on attached ClickHouse databases yet

statement error
CREATE TABLE ch.test_db.new_table AS SELECT 42 AS i;
----
not supported on attached ClickHouse databases yet

statement error
DROP TABLE ch.test_db.t1;
----
DROP is not supported on attached ClickHouse databases yet

statement error
ALTER TABLE ch.test_db.t1 ADD COLUMN extra INTEGER;
----
ALTER TABLE is not supported on attached ClickHouse databases yet

statement error
CREATE SCHEMA ch.new_db;
----
CREATE SCHEMA is not supported on attached ClickHouse databases yet

statement error
CREATE VIEW ch.test_db.new_view AS SELECT 42;
----
CREATE VIEW is not supported for ClickHouse databases

statement error
CREATE INDEX idx ON ch.test_db.t1(id);
----
Indexes are not supported for ClickHouse tables

statement error
MERGE INTO ch.test_db.t1 USING (VALUES (1)) src(id) ON t1.id = src.id
WHEN MATCHED THEN UPDATE SET name = 'x';
----
MERGE INTO is not supported for ClickHouse tables

query I
SELECT count(*) FROM ch.test_db.t1;
----
3
```

In `test/sql/attach/attach.test`, change the expected error of `CREATE SCHEMA ch.new_db;` from `clickhouse_scanner is read-only` to `CREATE SCHEMA is not supported on attached ClickHouse databases yet`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make release && make smoke ARGS='test/sql/write/*'`
Expected: FAIL. The writable-attach statements still report `clickhouse_scanner is read-only`.

- [ ] **Step 3: Replace the read-only helpers**

In `src/include/clickhouse_utils.hpp`, replace the `ThrowReadOnly` declaration with:
```cpp
	//! Throws the error for a write statement that attached ClickHouse databases do not support (yet)
	[[noreturn]] static void ThrowUnsupportedWrite(const string &statement);
	//! Throws the error for a write that reaches the extension on a database attached with READ_ONLY. DuckDB
	//! itself rejects write statements on such databases; this covers what DuckDB cannot see, e.g.
	//! clickhouse_execute()
	[[noreturn]] static void ThrowReadOnly(const string &database_name);
```

In `src/clickhouse_utils.cpp`, replace the `ThrowReadOnly` definition with:
```cpp
void ClickhouseUtils::ThrowUnsupportedWrite(const string &statement) {
	throw NotImplementedException("%s is not supported on attached ClickHouse databases yet; run it in ClickHouse "
	                              "with clickhouse_execute() instead",
	                              statement);
}

void ClickhouseUtils::ThrowReadOnly(const string &database_name) {
	throw PermissionException("Cannot write to ClickHouse database \"%s\": it is attached in read-only mode",
	                          database_name);
}
```

- [ ] **Step 4: Catalog and schema entry**

In `src/storage/clickhouse_catalog.cpp`, replace these bodies (keep the signatures):
```cpp
optional_ptr<CatalogEntry> ClickhouseCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	ClickhouseUtils::ThrowUnsupportedWrite("CREATE SCHEMA");
}

void ClickhouseCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	ClickhouseUtils::ThrowUnsupportedWrite("DROP SCHEMA");
}
```
```cpp
PhysicalOperator &ClickhouseCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                       PhysicalOperator &) {
	ClickhouseUtils::ThrowUnsupportedWrite("CREATE TABLE AS");
}

PhysicalOperator &ClickhouseCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
                                                optional_ptr<PhysicalOperator>) {
	// replaced in Task 3
	ClickhouseUtils::ThrowUnsupportedWrite("INSERT");
}

PhysicalOperator &ClickhouseCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
                                                PhysicalOperator &) {
	ClickhouseUtils::ThrowUnsupportedWrite("DELETE");
}

PhysicalOperator &ClickhouseCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
                                                PhysicalOperator &) {
	ClickhouseUtils::ThrowUnsupportedWrite("UPDATE");
}

PhysicalOperator &ClickhouseCatalog::PlanMergeInto(ClientContext &, PhysicalPlanGenerator &, LogicalMergeInto &,
                                                   PhysicalOperator &) {
	throw NotImplementedException("MERGE INTO is not supported for ClickHouse tables");
}

unique_ptr<LogicalOperator> ClickhouseCatalog::BindCreateIndex(Binder &, CreateStatement &, TableCatalogEntry &,
                                                               unique_ptr<LogicalOperator>) {
	// must throw here, before the base implementation's IndexBinder::BindCreateIndex() gets anywhere near
	// LogicalGet::bind_data: it assumes bind_data is a TableScanBindData and casts + writes through it,
	// which is type confusion against our ClickhouseScanBindData
	throw NotImplementedException("Indexes are not supported for ClickHouse tables");
}

unique_ptr<LogicalOperator> ClickhouseCatalog::BindAlterAddIndex(Binder &, TableCatalogEntry &,
                                                                 unique_ptr<LogicalOperator>,
                                                                 unique_ptr<CreateIndexInfo>,
                                                                 unique_ptr<AlterTableInfo>) {
	throw NotImplementedException("Indexes are not supported for ClickHouse tables");
}
```
Add `#include "duckdb/common/exception.hpp"` if `NotImplementedException` is not already visible.

In `src/storage/clickhouse_schema_entry.cpp`, replace every `ClickhouseUtils::ThrowReadOnly();` body as follows. First add this helper above the first method:
```cpp
[[noreturn]] static void ThrowNotSupported(const string &what) {
	throw NotImplementedException("%s cannot be created in a ClickHouse database", what);
}
```
Then:
- `CreateTable` → `ClickhouseUtils::ThrowUnsupportedWrite("CREATE TABLE");`
- `CreateView` → `throw NotImplementedException("CREATE VIEW is not supported for ClickHouse databases; create the view in ClickHouse with clickhouse_execute()");`
- `CreateFunction` → `ThrowNotSupported("Functions and macros");`
- `CreateIndex` → `throw NotImplementedException("Indexes are not supported for ClickHouse tables");`
- `CreateSequence` → `ThrowNotSupported("Sequences");`
- `CreateTableFunction` → `ThrowNotSupported("Table functions");`
- `CreateCopyFunction` → `ThrowNotSupported("Copy functions");`
- `CreatePragmaFunction` → `ThrowNotSupported("Pragma functions");`
- `CreateCollation` → `ThrowNotSupported("Collations");`
- `CreateType` → `ThrowNotSupported("Types");`
- `Alter` → `ClickhouseUtils::ThrowUnsupportedWrite("ALTER TABLE");`
- `DropEntry` → `ClickhouseUtils::ThrowUnsupportedWrite("DROP");`

- [ ] **Step 5: Transaction write tracking and the ROLLBACK warning**

`src/include/storage/clickhouse_transaction.hpp`: replace the `ClickhouseTransaction` class with:
```cpp
//! ClickHouse has no multi-statement transactions: a DuckDB transaction on an attached ClickHouse database holds no
//! remote state, every scan is an independent ClickHouse query and every write is committed as soon as it is sent.
//! The transaction only remembers whether it wrote, so a ROLLBACK can warn that those writes were not undone.
class ClickhouseTransaction : public Transaction {
public:
	ClickhouseTransaction(TransactionManager &manager, ClientContext &context);
	~ClickhouseTransaction() override;

	static ClickhouseTransaction &Get(ClientContext &context, Catalog &catalog);

	void MarkWritten() {
		written = true;
	}
	bool HasWritten() const {
		return written;
	}

private:
	atomic<bool> written {false};
};
```
Add `#include "duckdb/common/atomic.hpp"` to the header.

`src/storage/clickhouse_transaction.cpp`: add the includes `#include "duckdb/logging/logger.hpp"`, `#include "duckdb/main/attached_database.hpp"` and `#include "duckdb/common/string_util.hpp"`. Add:
```cpp
ClickhouseTransaction &ClickhouseTransaction::Get(ClientContext &context, Catalog &catalog) {
	return Transaction::Get(context, catalog).Cast<ClickhouseTransaction>();
}
```
Replace `RollbackTransaction` with:
```cpp
void ClickhouseTransactionManager::RollbackTransaction(Transaction &transaction) {
	auto &clickhouse_transaction = transaction.Cast<ClickhouseTransaction>();
	if (clickhouse_transaction.HasWritten()) {
		auto context = clickhouse_transaction.context.lock();
		if (context) {
			DUCKDB_LOG_WARNING(*context, StringUtil::Format("ClickHouse writes made in this transaction were already "
			                                                "committed and cannot be rolled back (database \"%s\")",
			                                                db.GetName()));
		}
	}
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
}
```
`TransactionManager` exposes the attached database as `db`. If the member is named differently in v1.5.4 (see `duckdb/src/include/duckdb/transaction/transaction_manager.hpp`), use it. `Transaction::context` is a `weak_ptr<ClientContext>`.

- [ ] **Step 6: README**

In `README.md`:
- Replace the intro phrase "as a read-only DuckDB database" with "as a DuckDB database you can query and write to".
- Replace the Limitations bullet "- Read-only: `INSERT`, `UPDATE`, `DELETE` and DDL are rejected." with:
```markdown
- `UPDATE`, `DELETE` and DDL (`CREATE`/`DROP`/`ALTER`) are not supported yet; run them with `clickhouse_execute()`.
  `MERGE INTO`, indexes and `CREATE VIEW` are not supported.
- Attach with `(TYPE clickhouse, READ_ONLY)` to reject every write.
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `make release && make smoke`
Expected: `All tests passed`. `test/sql/write/*` passes. The no-server suite (`./build/release/test/unittest "test/*"`) passes too.
If DuckDB rejects a statement in the binder with its own message before our hook runs (possible for `ALTER TABLE` or CTAS), accept DuckDB's message in `unsupported.test` and list it in the report. The final `count(*) = 3` still proves nothing was written.

- [ ] **Step 8: Commit**

```bash
git add -A src test README.md
git commit -m "feat: attached ClickHouse databases are writable by default; READ_ONLY opt-in, rollback warning

<your harness's Co-Authored-By trailer>"
```

---
### Task 2: clickhouse-cpp insert patch, connection write API and `clickhouse_execute()`

**Files:**
- Create: `vcpkg_ports/clickhouse-cpp/insert-query-settings.patch`
- Modify: `vcpkg_ports/clickhouse-cpp/portfile.cmake`, `vcpkg_ports/clickhouse-cpp/vcpkg.json`
- Modify: `src/include/clickhouse_connection.hpp`, `src/clickhouse_connection.cpp`
- Modify: `src/storage/clickhouse_connection_pool.cpp`, `src/include/storage/clickhouse_connection_pool.hpp` (comment only)
- Modify: `src/include/clickhouse_utils.hpp`, `src/clickhouse_utils.cpp`, `src/clickhouse_query.cpp`
- Create: `src/include/clickhouse_execute.hpp`, `src/clickhouse_execute.cpp`
- Modify: `src/CMakeLists.txt`, `src/clickhouse_scanner_extension.cpp`, `README.md`
- Create: `test/sql/write/execute.test`

**Interfaces:**
- Consumes (Task 1):
  - `ClickhouseUtils::ThrowReadOnly(const string &database_name)`
  - `ClickhouseTransaction::Get(ClientContext &, Catalog &)` and `MarkWritten()`
  - the ROLLBACK warning
- Produces:
  - `clickhouse::Client::BeginInsert(const clickhouse::Query &)` (patched clickhouse-cpp)
  - On `ClickhouseConnection`: `bool Execute(const string &sql)`, `clickhouse::Block BeginInsert(const string &sql)`, `void SendInsertBlock(const clickhouse::Block &block)`, `void EndInsert()`, `void AbortInsert()`, `bool IsInserting() const`
  - `static string ClickhouseUtils::StripTrailingSemicolons(string sql)`
  - The `clickhouse_execute` table function (`ClickhouseExecuteFunction`)

- [ ] **Step 1: Write the failing test**

`test/sql/write/execute.test`:
```
# name: test/sql/write/execute.test
# description: clickhouse_execute runs ClickHouse statements, clears the metadata cache and counts as a write
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE IF EXISTS w_exec');

# load the schema list first, so the next check proves clickhouse_execute clears it
query I
SELECT count(*) FROM duckdb_schemas() WHERE database_name = 'ch' AND schema_name = 'w_exec';
----
0

query I
CALL clickhouse_execute('ch', 'CREATE DATABASE w_exec');
----
true

query I
SELECT count(*) FROM duckdb_schemas() WHERE database_name = 'ch' AND schema_name = 'w_exec';
----
1

statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_exec.t (id UInt32, s String) ENGINE = MergeTree ORDER BY id');

# trailing semicolons are ignored
statement ok
CALL clickhouse_execute('ch', 'INSERT INTO w_exec.t VALUES (1, ''a''), (2, ''b'');');

query II
SELECT * FROM ch.w_exec.t ORDER BY id;
----
1	a
2	b

# the SELECT * FROM form works too, and ALTERs are visible right away
query I
SELECT Success FROM clickhouse_execute('ch', 'ALTER TABLE w_exec.t ADD COLUMN extra UInt8 DEFAULT 7');
----
true

query III
SELECT * FROM ch.w_exec.t ORDER BY id;
----
1	a	7
2	b	7

statement error
CALL clickhouse_execute('ch', 'SELECT 1');
----
use clickhouse_query()

statement error
CALL clickhouse_execute('ch', 'THIS IS NOT SQL');
----
ClickHouse error 62 (SYNTAX_ERROR)

statement error
CALL clickhouse_execute('ch', NULL);
----
cannot be NULL

statement error
CALL clickhouse_execute('ch', '  ;  ');
----
cannot be empty

statement error
CALL clickhouse_execute('memory', 'SELECT 1');
----
is not a ClickHouse database

statement error
CALL clickhouse_execute('nope', 'SELECT 1');
----
Failed to find attached database

# a READ_ONLY attachment rejects clickhouse_execute; nothing reaches ClickHouse
statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch_ro (TYPE clickhouse, READ_ONLY);

statement error
CALL clickhouse_execute('ch_ro', 'CREATE DATABASE w_exec_never');
----
attached in read-only mode

query I
SELECT count(*) FROM clickhouse_query('ch', 'SELECT name FROM system.databases WHERE name = ''w_exec_never''');
----
0

# ROLLBACK cannot undo a ClickHouse write: the write stays and a warning is logged
statement ok
CALL enable_logging(level = 'warning');

statement ok
BEGIN;

statement ok
CALL clickhouse_execute('ch', 'INSERT INTO w_exec.t (id, s) VALUES (3, ''c'')');

statement ok
ROLLBACK;

query I
SELECT count(*) FROM ch.w_exec.t;
----
3

query II
SELECT log_level, message FROM duckdb_logs WHERE message LIKE '%cannot be rolled back%';
----
WARNING	ClickHouse writes made in this transaction were already committed and cannot be rolled back (database "ch")

# a transaction that only read rolls back silently
statement ok
CALL truncate_duckdb_logs();

statement ok
BEGIN;

query I
SELECT count(*) FROM ch.w_exec.t;
----
3

statement ok
ROLLBACK;

query I
SELECT count(*) FROM duckdb_logs WHERE message LIKE '%cannot be rolled back%';
----
0

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE w_exec');
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/write/execute.test`
Expected: FAIL with `Catalog Error: Table Function with name clickhouse_execute does not exist`.

- [ ] **Step 3: Patch clickhouse-cpp so `BeginInsert` sends query settings**

clickhouse-cpp 2.6.2's `BeginInsert(Query)` calls `SendQuery(query.GetText())`, which drops every setting. We need two settings to reach the server with an INSERT:
- the user's ATTACH `settings`;
- `low_cardinality_allow_in_native_format=0`, so the INSERT header and data use plain columns instead of LowCardinality.

Create `vcpkg_ports/clickhouse-cpp/insert-query-settings.patch` with exactly this content (keep the trailing newline):
```diff
diff --git a/clickhouse/client.cpp b/clickhouse/client.cpp
index b5d287b..a92c026 100644
--- a/clickhouse/client.cpp
+++ b/clickhouse/client.cpp
@@ -546,7 +546,7 @@ Block Client::Impl::BeginInsert(Query query) {
         return true;
     });
 
-    SendQuery(query.GetText());
+    SendQuery(query);
 
     // Wait for a data packet and return
     uint64_t server_packet = 0;
@@ -1381,6 +1381,10 @@ Block Client::BeginInsert(const std::string& query, const std::string& query_id)
     return impl_->BeginInsert(Query(query, query_id));
 }
 
+Block Client::BeginInsert(const Query& query) {
+    return impl_->BeginInsert(query);
+}
+
 void Client::SendInsertBlock(const Block& block) {
     impl_->SendInsertBlock(block);
 }
diff --git a/clickhouse/client.h b/clickhouse/client.h
index a55fd74..a0949cf 100644
--- a/clickhouse/client.h
+++ b/clickhouse/client.h
@@ -304,6 +304,8 @@ public:
     /// Start an \p INSERT statement, insert batches of data, then finish the insert.
     Block BeginInsert(const std::string& query);
     Block BeginInsert(const std::string& query, const std::string& query_id);
+    /// Same, but sends the query's settings (and id) along with it.
+    Block BeginInsert(const Query& query);
 
     /// Insert data using a \p block returned by \p BeginInsert.
     void SendInsertBlock(const Block& block);
```
In `vcpkg_ports/clickhouse-cpp/portfile.cmake`, add `insert-query-settings.patch` as the last entry under `PATCHES` (after `expose-low-cardinality-columns.patch`). In `vcpkg_ports/clickhouse-cpp/vcpkg.json`, change `"port-version": 2` to `"port-version": 3` so vcpkg rebuilds the port. Add a short paragraph for the new patch to `vcpkg_ports/clickhouse-cpp/README.md`, following the style of the existing entries.

- [ ] **Step 4: Connection write API**

In `src/include/clickhouse_connection.hpp`, add these public members after `Query()`:
```cpp
	//! Runs a statement that is not expected to return rows, e.g. DDL. Returns false -- after cancelling the rest
	//! of the result -- as soon as the statement returns a row, true once it has finished without returning any
	bool Execute(const string &sql);

	//! Starts an INSERT (`INSERT INTO … VALUES` or `INSERT INTO … SELECT … FROM input(…)`) with the connection's
	//! settings, and returns the server's header block: one empty column per value sent, in order
	clickhouse::Block BeginInsert(const string &sql);
	//! Sends one block of rows for the INSERT started by BeginInsert()
	void SendInsertBlock(const clickhouse::Block &block);
	//! Finishes the INSERT started by BeginInsert(); errors ClickHouse reports while writing surface here
	void EndInsert();
	//! Abandons the INSERT started by BeginInsert(). The native protocol cannot cancel an INSERT, so the connection is
	//! marked broken: the pool closes it instead of reusing it, and ClickHouse aborts the query when the socket
	//! closes. No-op when no INSERT is running
	void AbortInsert();
	bool IsInserting() const;
```
Update the `IsHealthy` doc comment to: `//! Usable for a new query: not broken, not mid-query or mid-insert, and answers a ping when it has been idle for 30s`.

In `src/clickhouse_connection.cpp`, add after `Query()`:
```cpp
bool ClickhouseConnection::Execute(const string &sql) {
	BeginQuery(sql);
	while (auto block = NextBlock()) {
		if (block->GetRowCount() > 0) {
			Cancel();
			return false;
		}
	}
	return true;
}

clickhouse::Block ClickhouseConnection::BeginInsert(const string &sql) {
	if (debug_print_queries) {
		Printer::Print(sql + "\n");
	}
	last_used = std::chrono::steady_clock::now();
	try {
		return client->BeginInsert(MakeQuery(sql));
	} catch (...) {
		// clickhouse-cpp stays in its "inserting" state after a failed BeginInsert, even for a server error
		// (e.g. an unknown table), so this connection cannot run anything else
		broken = true;
		RethrowAsDuckDBException(sql);
	}
}

void ClickhouseConnection::SendInsertBlock(const clickhouse::Block &block) {
	last_used = std::chrono::steady_clock::now();
	try {
		client->SendInsertBlock(block);
	} catch (...) {
		broken = true;
		RethrowAsDuckDBException(string());
	}
}

void ClickhouseConnection::EndInsert() {
	last_used = std::chrono::steady_clock::now();
	try {
		client->EndInsert();
	} catch (...) {
		broken = true;
		RethrowAsDuckDBException(string());
	}
}

void ClickhouseConnection::AbortInsert() {
	if (client->IsInserting()) {
		broken = true;
	}
}

bool ClickhouseConnection::IsInserting() const {
	return client->IsInserting();
}
```
`RethrowAsDuckDBException` resets nothing, so setting `broken = true` before it is enough. For a `ServerException`, it rethrows without touching `broken`.

In `IsHealthy()`, change the first condition to:
```cpp
	if (broken || client->IsSelecting() || client->IsInserting()) {
```

In `src/storage/clickhouse_connection_pool.cpp`, change the return of `TryRecoverConnection` to:
```cpp
	return !connection.IsBroken() && !connection.IsQueryRunning() && !connection.IsInserting();
```
Add this sentence to its comment in the header: `An INSERT cannot be cancelled, so a connection returned mid-insert is never recovered`.

- [ ] **Step 5: Share `StripTrailingSemicolons`**

Move `StripTrailingSemicolons` from `src/clickhouse_query.cpp` into `ClickhouseUtils`. Leave the body unchanged:
```cpp
	//! Removes trailing whitespace and semicolons, which the native protocol does not accept
	static string StripTrailingSemicolons(string sql);
```
Delete the static copy in `clickhouse_query.cpp` and call `ClickhouseUtils::StripTrailingSemicolons(...)` there. Add `#include "clickhouse_utils.hpp"` to `clickhouse_query.cpp`, and `#include "duckdb/common/string_util.hpp"` to `clickhouse_utils.cpp` if it is not already there.

- [ ] **Step 6: `clickhouse_execute`**

`src/include/clickhouse_execute.hpp`:
```cpp
#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! clickhouse_execute('<attached database>', '<ClickHouse statement>'): runs a statement that returns no rows
class ClickhouseExecuteFunction : public TableFunction {
public:
	ClickhouseExecuteFunction();
};

} // namespace duckdb
```

`src/clickhouse_execute.cpp`:
```cpp
#include "clickhouse_execute.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_transaction.hpp"

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
	auto database = DatabaseManager::Get(context).GetDatabase(context, database_name);
	if (!database) {
		throw BinderException("Failed to find attached database \"%s\" referenced in clickhouse_execute",
		                      database_name);
	}
	auto &catalog = database->GetCatalog();
	if (catalog.GetCatalogType() != ClickhouseCatalog::CATALOG_TYPE) {
		throw BinderException("Attached database \"%s\" is not a ClickHouse database", database_name);
	}
	if (database->IsReadOnly()) {
		ClickhouseUtils::ThrowReadOnly(database_name);
	}
	return catalog.Cast<ClickhouseCatalog>();
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
	ClickhouseTransaction::Get(context, catalog).MarkWritten();
	bool returned_no_rows;
	try {
		auto connection = catalog.GetConnectionPool().GetConnection();
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
```
Why `clickhouse_execute` skips an `enable_external_access` check: like `clickhouse_query`, it can only reach the connection pool of an already attached catalog.

Add `clickhouse_execute.cpp` to `src/CMakeLists.txt`, in alphabetical order after `clickhouse_error_codes.cpp`. In `src/clickhouse_scanner_extension.cpp`, add `#include "clickhouse_execute.hpp"` and `loader.RegisterFunction(ClickhouseExecuteFunction());` after `ClickhouseQueryFunction()`.

- [ ] **Step 7: README**

In the Functions table, add after the `clickhouse_query` row:
```markdown
| `clickhouse_execute(database, sql)` | Runs a ClickHouse statement that returns no rows (DDL, `ALTER`, mutations, `OPTIMIZE`, `SYSTEM …`) and clears that database's metadata cache. Returns `Success = true`. |
```
Add a `## Writing` section between `## Type mapping` and `## Filter and LIMIT pushdown`:
````markdown
## Writing

Attached databases are writable unless attached with `READ_ONLY`, which rejects every write, including
`clickhouse_execute`:

```sql
ATTACH '' AS ch (TYPE clickhouse, SECRET ch, READ_ONLY);
```

**Every write is committed immediately.** ClickHouse has no multi-statement transactions: each write statement is
sent to ClickHouse and committed when it runs. `COMMIT` does nothing, and `ROLLBACK` cannot undo writes that already
reached ClickHouse. A `ROLLBACK` after a ClickHouse write logs a warning, which is visible after
`CALL enable_logging(level = 'warning')` in `duckdb_logs`.

`clickhouse_execute(database, sql)` runs any ClickHouse statement that returns no rows. Afterwards, that database's
metadata cache is cleared, so the change is visible to DuckDB right away:

```sql
CALL clickhouse_execute('ch', 'CREATE TABLE analytics.daily (d Date, n UInt64) ENGINE = SummingMergeTree ORDER BY d');
```
````

- [ ] **Step 8: Run the tests to verify they pass**

Run: `make release && make smoke`
Expected: `All tests passed`, including `test/sql/write/execute.test`. The first build after the port change rebuilds clickhouse-cpp: expect a few extra minutes, and poll it in the background.
If `log_level` renders differently (e.g. lowercase), fix the expectation and note it in the report.

- [ ] **Step 9: Commit**

```bash
git add -A vcpkg_ports src test README.md
git commit -m "feat: clickhouse_execute(), connection insert API, send query settings with INSERTs

<your harness's Co-Authored-By trailer>"
```

---

### Task 3: `ClickhouseWriter` and `ClickhouseInsert` (INSERT / INSERT … SELECT / COPY for natively encoded types)

**Files:**
- Create: `src/include/clickhouse_writer.hpp`, `src/clickhouse_writer.cpp`
- Create: `src/include/storage/clickhouse_insert.hpp`, `src/storage/clickhouse_insert.cpp`
- Modify: `src/include/clickhouse_utils.hpp`, `src/clickhouse_utils.cpp`, `src/clickhouse_conversion.cpp` (move `PowerOfTen`/`ScaleTicks` to `ClickhouseUtils`)
- Modify: `src/include/clickhouse_types.hpp` (`ClickhouseColumnInfo::default_kind`), `src/storage/clickhouse_table_set.cpp`
- Modify: `src/storage/clickhouse_catalog.cpp` (`PlanInsert`)
- Modify: `src/CMakeLists.txt`, `src/storage/CMakeLists.txt`, `src/clickhouse_scanner_extension.cpp`, `README.md`
- Create: `test/sql/write/insert.test`

**Interfaces:**
- Consumes:
  - Task 2: `ClickhouseConnection::BeginInsert(sql) → clickhouse::Block`, `SendInsertBlock`, `EndInsert`, `AbortInsert`, `IsInserting`; `ClickhousePoolConnection::Invalidate()`; `clickhouse_execute`
  - Task 1: `ClickhouseTransaction::Get(context, catalog).MarkWritten()`
- Produces:
  - `enum class ClickhouseWriteMode { NATIVE, SERVER_CONVERSION, UNSUPPORTED }`
  - `static ClickhouseWriteMode ClickhouseWriter::GetWriteMode(const ClickhouseTypeNode &node)`
  - `static void ClickhouseWriter::AppendVector(Vector &source, idx_t count, const clickhouse::ColumnRef &target, const string &column_name)`
  - `struct ClickhouseInsertColumn { ClickhouseColumnInfo column; idx_t source_index; }`
  - `ClickhouseInsert::GetInsertColumns(ClickhouseTableEntry &, const physical_index_vector_t<idx_t> &)`
  - `ClickhouseInsert::BuildInsertQuery(ClickhouseTableEntry &, const vector<ClickhouseInsertColumn> &) → string` (Task 4 extends it)
  - `ClickhouseUtils::PowerOfTen(idx_t) → int64_t`, `ClickhouseUtils::ScaleTicks(int64_t ticks, idx_t from, idx_t to) → int64_t`
  - `ClickhouseColumnInfo::default_kind`
  - the `ch_insert_block_size` setting

- [ ] **Step 1: Write the failing test**

`test/sql/write/insert.test`:
```
# name: test/sql/write/insert.test
# description: INSERT, INSERT … SELECT and COPY into ClickHouse tables
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

require icu

statement ok
SET TimeZone = 'UTC';

# enable_time_time64_type lets clickhouse_execute create Time/Time64 columns
statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse, SETTINGS 'enable_time_time64_type=1');

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE IF EXISTS w_insert');

statement ok
CALL clickhouse_execute('ch', 'CREATE DATABASE w_insert');

# ---------------------------------------------------------------------------
# every natively written scalar type round-trips (same types as test_db.scalars, minus the server-converted ones)
# ---------------------------------------------------------------------------
statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.scalars (
    id UInt8, b Bool,
    i8 Int8, i16 Int16, i32 Int32, i64 Int64,
    u8 UInt8, u16 UInt16, u32 UInt32, u64 UInt64,
    i128 Int128, u128 UInt128,
    f32 Float32, f64 Float64,
    d9 Decimal(9, 2), d18 Decimal(18, 4), d38 Decimal(38, 10),
    s String, fs FixedString(3),
    d Date, d32 Date32,
    dt DateTime(''UTC''), dt64_3 DateTime64(3, ''UTC''), dt64_9 DateTime64(9, ''Asia/Tokyo''),
    uuid UUID,
    e8 Enum8(''red'' = 1, ''green'' = 2, ''blue'' = -3), e16 Enum16(''small'' = 1000, ''large'' = 2000)
) ENGINE = MergeTree ORDER BY id');

query I
INSERT INTO ch.w_insert.scalars SELECT * EXCLUDE (i256, u256, d76, ip4, ip6) FROM ch.test_db.scalars;
----
2

query I
SELECT count(*) FROM ch.w_insert.scalars;
----
2

query I
SELECT count(*) FROM (SELECT * FROM ch.w_insert.scalars EXCEPT SELECT * EXCLUDE (i256, u256, d76, ip4, ip6) FROM ch.test_db.scalars);
----
0

# ClickHouse's own view; DateTime64(9) keeps DuckDB's microseconds
query IIIIII
SELECT * FROM clickhouse_query('ch', 'SELECT id, toString(dt64_9), toString(e8), toString(uuid), hex(fs), toString(d32) FROM w_insert.scalars ORDER BY id');
----
1	2024-02-29 21:34:56.123456000	blue	61f0c404-5cb3-11e7-907b-a6006ad3dba0	616263	1900-01-01
2	1970-01-01 09:00:00.000000000	red	00000000-0000-0000-0000-000000000000	78797A	1970-01-01

# range limits; unlisted columns get ClickHouse defaults
query I
INSERT INTO ch.w_insert.scalars (id, i128, u128, d18, s, fs, d, d32, dt, dt64_3, dt64_9, uuid, e8) VALUES
    (3, 170141183460469231731687303715884105727, '340282366920938463463374607431768211455'::UHUGEINT,
     -99999999999999.9999, 'héllo', 'ab', DATE '2149-06-06', DATE '2299-12-31',
     TIMESTAMPTZ '2106-02-07 06:28:15+00', TIMESTAMPTZ '2299-12-31 23:59:59.999+00',
     TIMESTAMPTZ '2262-04-11 23:47:16.854775+00', 'ffffffff-ffff-ffff-ffff-ffffffffffff', 'green');
----
1

query IIIIIIIIIIIII
SELECT * FROM clickhouse_query('ch', 'SELECT toString(i128), toString(u128), toString(d18), s, hex(fs), toString(d), toString(d32), toString(dt), toString(dt64_3), toString(dt64_9, ''UTC''), toString(uuid), toString(e8), i32 FROM w_insert.scalars WHERE id = 3');
----
170141183460469231731687303715884105727	340282366920938463463374607431768211455	-99999999999999.9999	héllo	616200	2149-06-06	2299-12-31	2106-02-07 06:28:15	2299-12-31 23:59:59.999	2262-04-11 23:47:16.854775000	ffffffff-ffff-ffff-ffff-ffffffffffff	green	0

# ---------------------------------------------------------------------------
# Nullable and LowCardinality
# ---------------------------------------------------------------------------
statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.nullables (
    id UInt8, i Nullable(Int32), s Nullable(String), d Nullable(Date), dt Nullable(DateTime(''UTC'')),
    e Nullable(Enum8(''a'' = 1)), lc LowCardinality(Nullable(String)), lcs LowCardinality(String),
    dec Nullable(Decimal(10, 2))
) ENGINE = MergeTree ORDER BY id');

query I
INSERT INTO ch.w_insert.nullables SELECT * EXCLUDE (ip) FROM ch.test_db.nullables;
----
2

query I
SELECT count(*) FROM (SELECT * FROM ch.w_insert.nullables EXCEPT SELECT * EXCLUDE (ip) FROM ch.test_db.nullables);
----
0

query IIII
SELECT * FROM clickhouse_query('ch', 'SELECT id, i IS NULL, ifNull(lc, ''<null>''), lcs FROM w_insert.nullables ORDER BY id');
----
1	1	<null>	x
2	0	lc	y

# ---------------------------------------------------------------------------
# Array, Tuple and Map, including nested nulls
# ---------------------------------------------------------------------------
statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.nested (
    id UInt8, arr Array(Int32), arr_null Array(Nullable(String)), arr2 Array(Array(UInt8)),
    tup Tuple(a Int32, b String), tup_unnamed Tuple(Int32, String), m Map(String, UInt64),
    m_nested Map(String, Array(Nullable(Int32))), tup_nested Tuple(l Array(String), n Nullable(Int8))
) ENGINE = MergeTree ORDER BY id');

query I
INSERT INTO ch.w_insert.nested (id, arr, arr_null, arr2, tup, tup_unnamed, m)
SELECT id, arr, arr_null, arr2, tup, tup_unnamed, m FROM ch.test_db.nested;
----
2

query I
INSERT INTO ch.w_insert.nested VALUES
    (3, [4, 5], ['x', NULL], [[], [9, 8]], ROW(-1, 'two'), ROW(3, 'w'), MAP {'k': 42},
     MAP {'a': [1, NULL], 'b': []}, ROW(['p', 'q'], NULL));
----
1

query IIIIIIIII
SELECT * FROM ch.w_insert.nested ORDER BY id;
----
1	[1, 2, 3]	[a, NULL]	[[1], [2, 3]]	{'a': 1, 'b': x}	{'1': 2, '2': y}	{k1=1, k2=2}	{}	{'l': [], 'n': NULL}
2	[]	[]	[]	{'a': 0, 'b': ''}	{'1': 0, '2': ''}	{}	{}	{'l': [], 'n': NULL}
3	[4, 5]	[x, NULL]	[[], [9, 8]]	{'a': -1, 'b': two}	{'1': 3, '2': w}	{k=42}	{a=[1, NULL], b=[]}	{'l': [p, q], 'n': NULL}

query II
SELECT * FROM clickhouse_query('ch', 'SELECT toString(m_nested), toString(tup_nested) FROM w_insert.nested WHERE id = 3');
----
{'a':[1,NULL],'b':[]}	(['p','q'],NULL)

# ---------------------------------------------------------------------------
# Time / Time64
# ---------------------------------------------------------------------------
statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.times (id UInt8, t Time, t3 Time64(3), t9 Time64(9)) ENGINE = MergeTree ORDER BY id');

query I
INSERT INTO ch.w_insert.times SELECT * FROM ch.test_db.times;
----
2

query I
SELECT count(*) FROM (SELECT * FROM ch.w_insert.times EXCEPT SELECT * FROM ch.test_db.times);
----
0

query I
SELECT * FROM clickhouse_query('ch', 'SELECT toString(t9) FROM w_insert.times WHERE id = 1');
----
12:34:56.123456789

# ---------------------------------------------------------------------------
# column subsets: ClickHouse fills unlisted columns with their DEFAULTs; MATERIALIZED/ALIAS cannot be written
# ---------------------------------------------------------------------------
statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.defaults (
    id UInt32, s String DEFAULT ''dflt'', n UInt32 DEFAULT id * 10, m UInt32 MATERIALIZED id + 1,
    a UInt32 ALIAS id + 2, e String EPHEMERAL ''x''
) ENGINE = MergeTree ORDER BY id');

query I
INSERT INTO ch.w_insert.defaults (id) VALUES (1), (2);
----
2

query I
INSERT INTO ch.w_insert.defaults (s, id) VALUES ('given', 3);
----
1

query IIIII
SELECT id, s, n, m, a FROM ch.w_insert.defaults ORDER BY id;
----
1	dflt	10	2	3
2	dflt	20	3	4
3	given	30	4	5

statement error
INSERT INTO ch.w_insert.defaults (id, m) VALUES (4, 0);
----
is a MATERIALIZED column

statement error
INSERT INTO ch.w_insert.defaults VALUES (4, 's', 0, 0, 0);
----
is a MATERIALIZED column

# ---------------------------------------------------------------------------
# values ClickHouse cannot store are rejected, never clamped; nothing is written
# ---------------------------------------------------------------------------
statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.limits (
    id UInt8, d Date, d32 Date32, dt DateTime(''UTC''), dt64 DateTime64(3, ''UTC''), dt64_9 DateTime64(9, ''UTC''),
    fs FixedString(2), n Int32, arr Array(Int32), tup Tuple(a Int32, b String)
) ENGINE = MergeTree ORDER BY id');

statement error
INSERT INTO ch.w_insert.limits (id, d) VALUES (1, DATE '1969-12-31');
----
outside the range of ClickHouse type Date

statement error
INSERT INTO ch.w_insert.limits (id, d) VALUES (1, DATE '2149-06-07');
----
outside the range of ClickHouse type Date

statement error
INSERT INTO ch.w_insert.limits (id, d) VALUES (1, 'infinity'::DATE);
----
outside the range of ClickHouse type Date

statement error
INSERT INTO ch.w_insert.limits (id, d32) VALUES (1, DATE '1899-12-31');
----
outside the range of ClickHouse type Date32

statement error
INSERT INTO ch.w_insert.limits (id, dt) VALUES (1, TIMESTAMPTZ '2106-02-07 06:28:16+00');
----
outside the range of ClickHouse type DateTime

statement error
INSERT INTO ch.w_insert.limits (id, dt64) VALUES (1, TIMESTAMPTZ '1899-12-31 23:59:59+00');
----
outside the range of ClickHouse type DateTime64

statement error
INSERT INTO ch.w_insert.limits (id, dt64_9) VALUES (1, TIMESTAMPTZ '2262-04-12 00:00:00+00');
----
outside the range of ClickHouse type DateTime64

statement error
INSERT INTO ch.w_insert.limits (id, fs) VALUES (1, 'abc');
----
does not fit ClickHouse type FixedString(2)

statement error
INSERT INTO ch.w_insert.limits (id, n) VALUES (1, NULL);
----
is not Nullable

statement error
INSERT INTO ch.w_insert.limits (id, arr) VALUES (1, [1, NULL]);
----
is not Nullable

statement error
INSERT INTO ch.w_insert.limits (id, arr) VALUES (1, NULL);
----
is not Nullable

statement error
INSERT INTO ch.w_insert.limits (id, tup) VALUES (1, NULL);
----
is not Nullable

query I
SELECT count(*) FROM ch.w_insert.limits;
----
0

# a short FixedString value is zero-padded
query I
INSERT INTO ch.w_insert.limits (id, fs) VALUES (1, 'a');
----
1

query I
SELECT * FROM clickhouse_query('ch', 'SELECT hex(fs) FROM w_insert.limits');
----
6100

# ---------------------------------------------------------------------------
# column types that cannot be written natively
# ---------------------------------------------------------------------------
statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.unsupported (
    id UInt8, v Variant(String, UInt64), dyn Dynamic, agg AggregateFunction(sum, UInt64),
    sa SimpleAggregateFunction(anyLast, String), arr_ip Array(IPv4), m_ip Map(String, IPv4)
) ENGINE = Memory');

statement error
INSERT INTO ch.w_insert.unsupported (id, v) VALUES (1, 'x');
----
Cannot insert into ClickHouse column "v" of type Variant(String, UInt64)

statement error
INSERT INTO ch.w_insert.unsupported (id, dyn) VALUES (1, '"x"');
----
Cannot insert into ClickHouse column "dyn" of type Dynamic

statement error
INSERT INTO ch.w_insert.unsupported (id, agg) VALUES (1, 'x');
----
Cannot insert into ClickHouse column "agg" of type AggregateFunction(sum, UInt64)

statement error
INSERT INTO ch.w_insert.unsupported (id, sa) VALUES (1, 'x');
----
Cannot insert into ClickHouse column "sa" of type SimpleAggregateFunction(anyLast, String)

statement error
INSERT INTO ch.w_insert.unsupported (id, arr_ip) VALUES (1, ['1.1.1.1']);
----
Cannot insert into ClickHouse column "arr_ip" of type Array(IPv4)

statement error
INSERT INTO ch.w_insert.unsupported (id, m_ip) VALUES (1, MAP {'a': '1.1.1.1'});
----
Cannot insert into ClickHouse column "m_ip" of type Map(String, IPv4)

# the columns that can be written still can
query I
INSERT INTO ch.w_insert.unsupported (id) VALUES (1);
----
1

query I
SELECT id FROM ch.w_insert.unsupported;
----
1

statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.converted (id UInt8, ip IPv4) ENGINE = MergeTree ORDER BY id');

statement error
INSERT INTO ch.w_insert.converted VALUES (1, '1.2.3.4');
----
is not supported yet

# ---------------------------------------------------------------------------
# many blocks, INSERT … SELECT from DuckDB, COPY, empty inserts
# ---------------------------------------------------------------------------
statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.big (n UInt64, s String) ENGINE = MergeTree ORDER BY n');

statement error
SET ch_insert_block_size = 0;
----
ch_insert_block_size must be greater than 0

statement ok
SET ch_insert_block_size = 1000;

query I
INSERT INTO ch.w_insert.big SELECT range, 'v' || range FROM range(10500);
----
10500

query III
SELECT count(*), sum(n), max(s) FROM ch.w_insert.big;
----
10500	55119750	v9999

statement ok
RESET ch_insert_block_size;

statement ok
CREATE TABLE local_rows AS SELECT range + 20000 AS n, 'local ' || range AS s FROM range(5);

query I
INSERT INTO ch.w_insert.big SELECT * FROM local_rows;
----
5

query I
INSERT INTO ch.w_insert.big SELECT * FROM local_rows WHERE n < 0;
----
0

statement ok
COPY (SELECT range + 30000 AS n, 'csv ' || range AS s FROM range(50)) TO '__TEST_DIR__/ch_rows.csv' (HEADER);

query I
COPY ch.w_insert.big FROM '__TEST_DIR__/ch_rows.csv';
----
50

query I
SELECT count(*) FROM ch.w_insert.big WHERE n >= 20000;
----
55

# the INSERT carried the connection's settings (the clickhouse-cpp patch)
statement ok
CALL clickhouse_execute('ch', 'SYSTEM FLUSH LOGS');

query II
SELECT * FROM clickhouse_query('ch', 'SELECT Settings[''enable_time_time64_type''], Settings[''low_cardinality_allow_in_native_format''] FROM system.query_log WHERE type = ''QueryFinish'' AND query LIKE ''INSERT INTO `w_insert`.`big`%'' ORDER BY event_time_microseconds DESC LIMIT 1');
----
1	0

# ---------------------------------------------------------------------------
# unsupported INSERT forms
# ---------------------------------------------------------------------------
statement error
INSERT INTO ch.w_insert.big VALUES (1, 'x') RETURNING n;
----
RETURNING is not supported for ClickHouse tables

# rejected by DuckDB's binder or by PlanInsert, whichever comes first
statement error
INSERT OR IGNORE INTO ch.w_insert.big VALUES (1, 'x');

# ---------------------------------------------------------------------------
# transactions: the write is committed right away; ROLLBACK warns
# ---------------------------------------------------------------------------
statement ok
CALL enable_logging(level = 'warning');

statement ok
BEGIN;

query I
INSERT INTO ch.w_insert.big VALUES (40000, 'in transaction');
----
1

query I
SELECT count(*) FROM ch.w_insert.big WHERE n = 40000;
----
1

statement ok
ROLLBACK;

query I
SELECT count(*) FROM ch.w_insert.big WHERE n = 40000;
----
1

query I
SELECT count(*) FROM duckdb_logs WHERE message LIKE '%cannot be rolled back (database "ch")%';
----
1

# ---------------------------------------------------------------------------
# server errors; the failed INSERT's connection is discarded, not reused mid-insert
# ---------------------------------------------------------------------------
statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch2 (TYPE clickhouse);

statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.gone (n UInt8) ENGINE = MergeTree ORDER BY n');

query I
SELECT count(*) FROM ch.w_insert.gone;
----
0

# dropped behind ch's back: ch's metadata cache still has the table
statement ok
CALL clickhouse_execute('ch2', 'DROP TABLE w_insert.gone');

statement error
INSERT INTO ch.w_insert.gone VALUES (1);
----
UNKNOWN_TABLE

query I
INSERT INTO ch.w_insert.big VALUES (50000, 'after error');
----
1

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE w_insert');
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/write/insert.test`
Expected: FAIL at the first INSERT with `INSERT is not supported on attached ClickHouse databases yet`.

- [ ] **Step 3: Share the tick helpers**

Move `PowerOfTen` and `ScaleTicks` out of `src/clickhouse_conversion.cpp` into `ClickhouseUtils`, keeping their bodies. `clickhouse_conversion.cpp` then calls `ClickhouseUtils::PowerOfTen` / `ClickhouseUtils::ScaleTicks`.
```cpp
	//! 10^exponent (exponent <= 18)
	static int64_t PowerOfTen(idx_t exponent);
	//! Rescales ticks between decimal precisions, flooring when precision is lost
	static int64_t ScaleTicks(int64_t ticks, idx_t from_precision, idx_t to_precision);
```

- [ ] **Step 4: Record `default_kind`**

In `src/include/clickhouse_types.hpp`, add to `ClickhouseColumnInfo`:
```cpp
	//! system.columns.default_kind for table columns: "", DEFAULT, MATERIALIZED or ALIAS (EPHEMERAL columns are not
	//! loaded). Empty for query results
	string default_kind;
```
In `src/storage/clickhouse_table_set.cpp`:
- Select `default_kind` as a fourth column: `SELECT table, name, type, default_kind FROM system.columns …`.
- Read it with `auto column_kinds = block[3]->As<clickhouse::ColumnString>();`.
- Set it on the created column:
```cpp
			auto column =
			    ClickhouseColumnInfo::Create(string(column_names->At(row)), string(column_types->At(row)));
			column.default_kind = string(column_kinds->At(row));
			tables.back().columns.push_back(std::move(column));
```

- [ ] **Step 5: `ClickhouseWriter`**

`src/include/clickhouse_writer.hpp`:
```cpp
#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/common/types/vector.hpp"

#include <clickhouse/columns/column.h>

namespace duckdb {

//! How values of a ClickHouse column are written
enum class ClickhouseWriteMode : uint8_t {
	//! Encoded by ClickhouseWriter in the column's own native representation
	NATIVE,
	//! Sent in the form the read path produces (text, or Float32 for BFloat16) and converted by ClickHouse:
	//! IPv4/6, (U)Int256, Decimal(P > 38), BFloat16, JSON and the geo types
	SERVER_CONVERSION,
	//! Cannot be written: Variant, Dynamic, Object, AggregateFunction, SimpleAggregateFunction, Nothing, and
	//! Array/Tuple/Map holding anything that is not NATIVE
	UNSUPPORTED
};

//! Converts DuckDB vectors into clickhouse-cpp columns for INSERTs: the mirror image of ClickhouseConversion
class ClickhouseWriter {
public:
	static ClickhouseWriteMode GetWriteMode(const ClickhouseTypeNode &node);
	//! Appends the first `count` rows of `source` to `target`, a column cloned from an INSERT header block.
	//! `column_name` names the column in errors. Throws ConversionException for values ClickHouse cannot store:
	//! out-of-range dates and timestamps, oversized FixedStrings, NULLs in non-Nullable columns
	static void AppendVector(Vector &source, idx_t count, const clickhouse::ColumnRef &target,
	                         const string &column_name);
};

} // namespace duckdb
```

`src/clickhouse_writer.cpp`:
```cpp
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
static const ClickhouseTypeNode &UnwrapNullable(const ClickhouseTypeNode &node) {
	if ((node.name == "Nullable" || node.name == "LowCardinality") && node.children.size() == 1) {
		return UnwrapNullable(node.children[0]);
	}
	return node;
}

ClickhouseWriteMode ClickhouseWriter::GetWriteMode(const ClickhouseTypeNode &node) {
	static const unordered_set<string> NATIVE_TYPES = {
	    "Bool",   "Int8",   "Int16",       "Int32", "Int64",  "UInt8",    "UInt16",     "UInt32", "UInt64",
	    "Int128", "UInt128", "Float32",    "Float64", "String", "FixedString", "Date",   "Date32", "DateTime",
	    "DateTime64", "Time", "Time64",   "UUID",  "Enum8",  "Enum16"};
	static const unordered_set<string> CONVERTED_TYPES = {
	    "IPv4", "IPv6", "Int256", "UInt256", "BFloat16", "JSON", "Point", "Ring", "LineString", "MultiLineString",
	    "Polygon", "MultiPolygon"};
	auto &type = UnwrapNullable(node);
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

//===--------------------------------------------------------------------===//
// Errors
//===--------------------------------------------------------------------===//
[[noreturn]] static void ThrowMismatch(const Vector &source, const ch::ColumnRef &target, const string &column_name) {
	throw InternalException("Cannot write DuckDB %s values into ClickHouse column \"%s\" of type %s",
	                        source.GetType().ToString(), column_name, target->Type()->GetName());
}

[[noreturn]] static void ThrowNull(const ch::ColumnRef &target, const string &column_name) {
	throw ConversionException("Cannot insert NULL into ClickHouse column \"%s\": its type %s is not Nullable",
	                          column_name, target->Type()->GetName());
}

[[noreturn]] static void ThrowOutOfRange(const string &value, const ch::ColumnRef &target,
                                         const string &column_name) {
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

static string TimestampText(timestamp_tz_t value) {
	return Timestamp::ToString(timestamp_t(value.value)) + "+00";
}

//! `micros` expressed with `precision` decimal digits (floored); false when that overflows int64
static bool TryScaleMicros(int64_t micros, idx_t precision, int64_t &result) {
	if (precision <= 6) {
		result = ClickhouseUtils::ScaleTicks(micros, 6, precision);
		return true;
	}
	return TryMultiplyOperator::Operation<int64_t, int64_t, int64_t>(
	    micros, ClickhouseUtils::PowerOfTen(precision - 6), result);
}

static void AppendTimestamp(const AppendInput &input, const ch::ColumnRef &target) {
	if (input.source.GetType().id() != LogicalTypeId::TIMESTAMP_TZ) {
		ThrowMismatch(input.source, target, input.column_name);
	}
	if (auto typed = target->As<ch::ColumnDateTime>()) {
		// seconds since the epoch as UInt32: 1970-01-01 00:00:00 to 2106-02-07 06:28:15
		ForEachValue<timestamp_tz_t>(
		    input, target,
		    [&](timestamp_tz_t value) {
			    auto seconds = ClickhouseUtils::ScaleTicks(value.value, 6, 0);
			    if (seconds < 0 || seconds > NumericLimits<uint32_t>::Maximum()) {
				    ThrowOutOfRange(TimestampText(value), target, input.column_name);
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
		auto precision = typed->GetPrecision();
		ForEachValue<timestamp_tz_t>(
		    input, target,
		    [&](timestamp_tz_t value) {
			    int64_t ticks;
			    if (value.value < MIN_MICROS || value.value > MAX_MICROS ||
			        !TryScaleMicros(value.value, precision, ticks)) {
				    ThrowOutOfRange(TimestampText(value), target, input.column_name);
			    }
			    typed->Append(ticks);
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
			        "Cannot insert \"%s\" into ClickHouse column \"%s\": it is not a label of ClickHouse type %s", label,
			        input.column_name, target->Type()->GetName());
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
		AppendInput elements {ListVector::GetEntry(input.source), ListVector::GetListSize(input.source),
		                      element_sel, total, false, input.column_name};
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
		AppendInput child {*children[c], input.size, input.sel, input.count, input.nulls_as_default,
		                   input.column_name};
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
```
If a clickhouse-cpp or DuckDB call does not compile as written (e.g. an `absl` helper's exact name, or `EnumType::BeginValueToName`), look it up in the installed headers under `build/release/vcpkg_installed/*/include/clickhouse/` or in the DuckDB submodule, and keep the behaviour the same.

- [ ] **Step 6: `ClickhouseInsert`**

`src/include/storage/clickhouse_insert.hpp`:
```cpp
#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/common/index_vector.hpp"
#include "duckdb/execution/physical_operator.hpp"

namespace duckdb {
class ClickhouseTableEntry;

//! One column an INSERT writes: the ClickHouse column, and the input chunk column holding its values
struct ClickhouseInsertColumn {
	ClickhouseColumnInfo column;
	idx_t source_index;
};

//! INSERT, INSERT … SELECT and COPY … FROM into a ClickHouse table: streams the input into one native INSERT on one
//! pooled connection, a block every ch_insert_block_size rows
class ClickhouseInsert : public PhysicalOperator {
public:
	ClickhouseInsert(PhysicalPlan &physical_plan, LogicalOperator &op, ClickhouseTableEntry &table,
	                 vector<ClickhouseInsertColumn> columns);

	ClickhouseTableEntry &table;
	//! The inserted columns, in table order
	vector<ClickhouseInsertColumn> columns;
	//! The statement BeginInsert() runs, e.g. INSERT INTO `db`.`t` (`a`, `b`) VALUES
	string insert_sql;

	//! The columns an INSERT with this column_index_map writes (every column when the map is empty), in table order.
	//! Throws for columns that cannot be inserted into
	static vector<ClickhouseInsertColumn> GetInsertColumns(ClickhouseTableEntry &table,
	                                                       const physical_index_vector_t<idx_t> &column_index_map);
	static string BuildInsertQuery(ClickhouseTableEntry &table, const vector<ClickhouseInsertColumn> &columns);

public:
	// Source interface
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;
	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;
	bool IsSink() const override {
		return true;
	}
	bool ParallelSink() const override {
		return false;
	}

	string GetName() const override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;
};

} // namespace duckdb
```

`src/storage/clickhouse_insert.cpp`:
```cpp
#include "storage/clickhouse_insert.hpp"

#include "clickhouse_utils.hpp"
#include "clickhouse_writer.hpp"
#include "duckdb/common/exception.hpp"
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
```
Check the details against DuckDB v1.5.4's `PhysicalOperator` interface in `duckdb/src/include/duckdb/execution/physical_operator.hpp`, and against duckdb-postgres v1.5's `PostgresInsert`, which this operator mirrors.
- If a signature differs (e.g. `ParamsToString`), follow the header.
- If moving a new `ClickhousePoolConnection` into `connection` does not return the old connection to the pool, drop that line and let the state's destruction return it.

- [ ] **Step 7: `PlanInsert` and the setting**

In `src/storage/clickhouse_catalog.cpp`, replace the Task 1 `PlanInsert` stub. Add the includes `duckdb/planner/operator/logical_insert.hpp`, `duckdb/execution/physical_plan_generator.hpp`, `storage/clickhouse_insert.hpp` and `storage/clickhouse_table_entry.hpp` if they are missing.
```cpp
PhysicalOperator &ClickhouseCatalog::PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
                                                optional_ptr<PhysicalOperator> plan) {
	if (op.return_chunk) {
		throw NotImplementedException("RETURNING is not supported for ClickHouse tables");
	}
	if (op.on_conflict_info.action_type != OnConflictAction::THROW) {
		throw NotImplementedException("ON CONFLICT is not supported for ClickHouse tables");
	}
	D_ASSERT(plan);
	// DuckDB's own planner adds a projection filling in the DEFAULTs of unlisted columns
	// (ResolveDefaultsProjection); it is skipped on purpose, so ClickHouse applies its own defaults instead
	auto &table = op.table.Cast<ClickhouseTableEntry>();
	auto columns = ClickhouseInsert::GetInsertColumns(table, op.column_index_map);
	auto &insert = planner.Make<ClickhouseInsert>(op, table, std::move(columns));
	insert.children.push_back(*plan);
	return insert;
}
```
Add `clickhouse_writer.cpp` to `src/CMakeLists.txt` and `clickhouse_insert.cpp` to `src/storage/CMakeLists.txt`, keeping alphabetical order. Leave each `target_compile_features(... cxx_std_17)` line as it is.

In `src/clickhouse_scanner_extension.cpp`, add a validation callback next to `SetClickhouseDebugPrintQueries`:
```cpp
static void SetClickhouseInsertBlockSize(ClientContext &context, SetScope scope, Value &parameter) {
	if (UBigIntValue::Get(parameter) == 0) {
		throw InvalidInputException("ch_insert_block_size must be greater than 0");
	}
}
```
Register the setting after `ch_order_pushdown`:
```cpp
	config.AddExtensionOption("ch_insert_block_size", "Rows per block sent to ClickHouse during INSERT",
	                          LogicalType::UBIGINT, Value::UBIGINT(65536), SetClickhouseInsertBlockSize);
```

- [ ] **Step 8: README**

In `## Writing`, insert this after the paragraph about writes being committed immediately and before the `clickhouse_execute` paragraph:
````markdown
`INSERT` (with `VALUES` or a `SELECT`) and `COPY … FROM` stream rows into ClickHouse over the native protocol, in
blocks of `ch_insert_block_size` rows:

```sql
INSERT INTO ch.analytics.events SELECT * FROM read_parquet('events/*.parquet');
COPY ch.analytics.events FROM 'events.csv';
```

- **Defaults:** only the listed columns are sent, so ClickHouse fills the others with their `DEFAULT` expressions.
  `MATERIALIZED` and `ALIAS` columns cannot be inserted into: list the other columns explicitly.
- **Out-of-range values:** values ClickHouse cannot store are rejected, never clamped:
  - dates and timestamps outside the range of `Date`, `Date32`, `DateTime` or `DateTime64`;
  - strings longer than a `FixedString`;
  - `NULL` in a non-`Nullable` column, including `Array`, `Tuple` and `Map` columns, which ClickHouse cannot make
    `Nullable`.
- **Precision:** timestamps and times are written at the column's precision, and anything finer is truncated.
- **Atomicity:** an `INSERT` is not atomic. If it fails part-way, ClickHouse may already have committed the blocks sent
  so far. On replicated tables, ClickHouse deduplicates identical blocks by default; attach with
  `SETTINGS 'insert_deduplicate=0'` to turn that off.
- **Unsupported:**
  - `RETURNING` and `ON CONFLICT`;
  - columns of type `Variant`, `Dynamic`, `AggregateFunction` or `SimpleAggregateFunction`, or nested types holding a
    type that ClickHouse converts (e.g. `Array(IPv4)`). Leave these columns out of the column list, or use
    `clickhouse_execute`;
  - columns ClickHouse converts on read (`IPv4/6`, `(U)Int256`, `Decimal256`, `JSON`, geo types) are not supported
    **yet**.
````
Add a Settings table row after `ch_order_pushdown`:
```markdown
| `ch_insert_block_size` | `65536` | Rows per block sent to ClickHouse during `INSERT` |
```

- [ ] **Step 9: Run the tests to verify they pass**

Run in the background, and poll every few minutes:
```bash
make release && make smoke
```
Expected: `All tests passed`, including `test/sql/write/insert.test`. Then run the debug build, whose assertions catch vector-handling mistakes:
```bash
make debug && make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*'
```
Expected: `All tests passed`.

- [ ] **Step 10: Commit**

```bash
git add -A src test README.md
git commit -m "feat: INSERT, INSERT … SELECT and COPY into attached ClickHouse tables

<your harness's Co-Authored-By trailer>"
```

---

### Task 4: Server-converted column types through `input()`

**Files:**
- Modify: `src/include/clickhouse_writer.hpp`, `src/clickhouse_writer.cpp`
- Modify: `src/storage/clickhouse_insert.cpp`
- Modify: `test/sql/write/insert.test` (remove the `w_insert.converted` block)
- Create: `test/sql/write/insert_converted.test`
- Modify: `README.md`

**Interfaces:**
- Consumes (Task 3): `ClickhouseWriter::GetWriteMode`, `ClickhouseWriteMode::SERVER_CONVERSION`, `ClickhouseInsert::GetInsertColumns`, `ClickhouseInsert::BuildInsertQuery`
- Produces:
  - `static string ClickhouseWriter::ServerInputType(const ClickhouseTypeNode &node)`
  - `static string ClickhouseWriter::ServerConversion(const ClickhouseTypeNode &node, const string &expr)`

**Why:** the read path has ClickHouse convert some types into a form DuckDB decodes: `toString`, `wkt` and `toJSONString` produce text, and `toFloat32` produces floats. DuckDB therefore sees these columns as VARCHAR, JSON or FLOAT. To write them, we send that same form and have ClickHouse convert it back:
```sql
INSERT INTO `db`.`t` (`id`, `ip`) SELECT c1, CAST(c2 AS Nullable(IPv4)) FROM input('c1 UInt8, c2 Nullable(String)')
```
`BeginInsert` then returns a header shaped like `input()`'s structure. `ClickhouseWriter` fills it through its existing String and Float32 paths.

**This task starts with a spike.** The spec (§4) leaves open whether clickhouse-cpp's `BeginInsert` works with `INSERT … SELECT … FROM input(…)`.
- Try the form above first. If the server rejects it or never sends a header block, try `INSERT INTO … (cols) FORMAT Native SELECT … FROM input(…)`.
- If neither works, **stop and report BLOCKED** with the server's responses. Do not build a workaround. The fallback (reject these types, and document it) is the controller's decision.

- [ ] **Step 1: Write the failing test**

Delete this block from `test/sql/write/insert.test`, since it moves into the new file:
```
statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_insert.converted (id UInt8, ip IPv4) ENGINE = MergeTree ORDER BY id');

statement error
INSERT INTO ch.w_insert.converted VALUES (1, '1.2.3.4');
----
is not supported yet
```

`test/sql/write/insert_converted.test`:
```
# name: test/sql/write/insert_converted.test
# description: INSERT into column types ClickHouse converts: values travel as the read path's text and are converted back
# group: [write]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE IF EXISTS w_conv');

statement ok
CALL clickhouse_execute('ch', 'CREATE DATABASE w_conv');

statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_conv.source (
    id UInt8, ip4 IPv4, ip6 Nullable(IPv6), i256 Int256, u256 UInt256, d76 Decimal(76, 5), bf BFloat16, j JSON,
    p Point, r Ring, ls LineString, mls MultiLineString, poly Polygon, mpoly MultiPolygon, n Int32
) ENGINE = MergeTree ORDER BY id');

statement ok
CALL clickhouse_execute('ch', 'CREATE TABLE w_conv.converted AS w_conv.source');

statement ok
CALL clickhouse_execute('ch', 'INSERT INTO w_conv.source VALUES
    (1, ''192.168.0.1'', ''2001:db8::1'', -1, 1, 12345.67891, 1.5, ''{"a": "x", "b": {"c": "y"}}'',
     (1, 2), [(0, 0), (1, 0), (1, 1), (0, 0)], [(0, 0), (1, 1)], [[(0, 0), (1, 1)], [(2, 2), (3, 3)]],
     [[(0, 0), (4, 0), (4, 4), (0, 0)]], [[[(0, 0), (1, 0), (1, 1), (0, 0)]]], 7),
    (2, ''0.0.0.0'', NULL, 0, 0, 0, 0, ''{}'', (0, 0), [(0, 0), (2, 0), (2, 2), (0, 0)], [(5, 5), (6, 6)],
     [[(1, 1), (2, 2)]], [[(0, 0), (3, 0), (3, 3), (0, 0)]], [[[(0, 0), (2, 0), (2, 2), (0, 0)]]], 0)');

# ClickHouse -> DuckDB -> ClickHouse
query I
INSERT INTO ch.w_conv.converted SELECT * FROM ch.w_conv.source;
----
2

# DuckDB's view: identical
query I
SELECT count(*) FROM (SELECT * FROM ch.w_conv.converted EXCEPT SELECT * FROM ch.w_conv.source);
----
0

# ClickHouse's own view: identical values, compared as ClickHouse types
query I
SELECT * FROM clickhouse_query('ch', 'SELECT count() FROM w_conv.source AS s INNER JOIN w_conv.converted AS c ON s.id = c.id
    WHERE s.ip4 = c.ip4 AND ((s.ip6 IS NULL AND c.ip6 IS NULL) OR s.ip6 = c.ip6) AND s.i256 = c.i256 AND s.u256 = c.u256
    AND s.d76 = c.d76 AND s.bf = c.bf AND toJSONString(s.j) = toJSONString(c.j) AND wkt(s.p) = wkt(c.p)
    AND wkt(s.r) = wkt(c.r) AND wkt(s.ls) = wkt(c.ls) AND wkt(s.mls) = wkt(c.mls) AND wkt(s.poly) = wkt(c.poly)
    AND wkt(s.mpoly) = wkt(c.mpoly) AND s.n = c.n');
----
2

# literal values, a column subset and a NULL
query I
INSERT INTO ch.w_conv.converted (id, ip4, ip6, i256, d76, j, p) VALUES
    (3, '10.0.0.1', NULL, '-57896044618658097711785492504343953926634992332820282019728792003956564819968', '-1.5',
     '{"k": "v"}', 'POINT (3 4)');
----
1

query IIIIII
SELECT * FROM clickhouse_query('ch', 'SELECT toString(ip4), ip6 IS NULL, toString(i256), toString(d76), toJSONString(j), wkt(p) FROM w_conv.converted WHERE id = 3');
----
10.0.0.1	1	-57896044618658097711785492504343953926634992332820282019728792003956564819968	-1.5	{"k":"v"}	POINT(3 4)

# ClickHouse rejects text it cannot convert; nothing is written and the next INSERT works
statement error
INSERT INTO ch.w_conv.converted (id, ip4) VALUES (4, 'not an ip');
----
ClickHouse error

query I
SELECT count(*) FROM ch.w_conv.converted WHERE id = 4;
----
0

query I
INSERT INTO ch.w_conv.converted (id, ip4) VALUES (5, '1.1.1.1');
----
1

# natively written columns keep working alongside converted ones, including many blocks
statement ok
SET ch_insert_block_size = 100;

query I
INSERT INTO ch.w_conv.converted (id, ip4, n) SELECT 10 + (range % 200), '10.0.' || (range // 256) || '.' || (range % 256), range::INTEGER FROM range(1000);
----
1000

query II
SELECT * FROM clickhouse_query('ch', 'SELECT count(), sum(n) FROM w_conv.converted WHERE id >= 10');
----
1000	499500

statement ok
CALL clickhouse_execute('ch', 'DROP DATABASE w_conv');
```
If the server needs a setting to create `BFloat16`, `JSON` or geo columns, add it to the ATTACH, e.g. `ATTACH '…' AS ch (TYPE clickhouse, SETTINGS 'allow_experimental_bfloat16_type=1')`. ClickHouse's error names the setting. Report any such setting.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/write/insert_converted.test`
Expected: FAIL at the first INSERT with `is not supported yet`.

- [ ] **Step 3: Conversion expressions**

In `src/include/clickhouse_writer.hpp`, add to `ClickhouseWriter`:
```cpp
	//! For a SERVER_CONVERSION column: the input() column type its values are sent as -- (Nullable) String, or
	//! (Nullable) Float32 for BFloat16, matching what the read path produces for it
	static string ServerInputType(const ClickhouseTypeNode &node);
	//! For a SERVER_CONVERSION column: the ClickHouse expression converting `expr` (of ServerInputType) to the
	//! column's own type
	static string ServerConversion(const ClickhouseTypeNode &node, const string &expr);
```
In `src/clickhouse_writer.cpp`, add after `GetWriteMode`:
```cpp
string ClickhouseWriter::ServerInputType(const ClickhouseTypeNode &node) {
	auto base = UnwrapNullable(node).name == "BFloat16" ? "Float32" : "String";
	return ClickhouseTypes::IsNullable(node) ? "Nullable(" + string(base) + ")" : string(base);
}

string ClickhouseWriter::ServerConversion(const ClickhouseTypeNode &node, const string &expr) {
	// geo types are read as WKT (wkt()); CAST cannot parse WKT, the readWKT* functions can
	static const unordered_map<string, string> WKT_READERS = {
	    {"Point", "readWKTPoint"},           {"Ring", "readWKTRing"},       {"LineString", "readWKTLineString"},
	    {"MultiLineString", "readWKTMultiLineString"}, {"Polygon", "readWKTPolygon"},
	    {"MultiPolygon", "readWKTMultiPolygon"}};
	auto reader = WKT_READERS.find(UnwrapNullable(node).name);
	if (reader != WKT_READERS.end()) {
		return reader->second + "(" + expr + ")";
	}
	// the column's full type, wrappers included: CAST(Nullable(String) AS Nullable(IPv4)) keeps NULLs
	return "CAST(" + expr + " AS " + node.text + ")";
}
```
Add `#include "duckdb/common/unordered_map.hpp"`.

- [ ] **Step 4: The `input()` form of the INSERT**

In `src/storage/clickhouse_insert.cpp`:
1. In `GetInsertColumns`, replace the `SERVER_CONVERSION` case with:
```cpp
		case ClickhouseWriteMode::NATIVE:
		case ClickhouseWriteMode::SERVER_CONVERSION:
			break;
```
Remove the old `NATIVE: break;` case so each enumerator appears once.
2. Replace `BuildInsertQuery` with:
```cpp
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
	auto target = "INSERT INTO " + ClickhouseUtils::QuoteIdentifier(table.schema.name) + "." +
	              ClickhouseUtils::QuoteIdentifier(table.name) + " (" + StringUtil::Join(names, ", ") + ")";
	if (!server_conversion) {
		return target + " VALUES";
	}
	// values ClickHouse converts are sent as input() columns and converted by the SELECT; the header BeginInsert()
	// returns then describes input()'s structure, which ClickhouseWriter fills like any other INSERT
	return target + " SELECT " + StringUtil::Join(select_list, ", ") + " FROM input(" +
	       ClickhouseUtils::QuoteLiteral(StringUtil::Join(input_columns, ", ")) + ")";
}
```
`ClickhouseUtils::QuoteLiteral` escapes the single quotes inside types such as `DateTime64(3, 'UTC')` or `Enum8('a' = 1)`.

- [ ] **Step 5: Run the spike test, then the full suite**

Run: `make release && make smoke ARGS='test/sql/write/insert_converted.test'`
- If the INSERT fails with a server syntax error or hangs, apply the spike rule above: try the `FORMAT Native` variant, and if that fails too, report BLOCKED. Kill a hung run after 5 minutes.
- If the geo round trip fails only because `wkt()` output is not accepted by a `readWKT*` function, report which type and the error, and ask the controller before changing anything.

Then run `make smoke`, and afterwards `make debug && make smoke SMOKE_BUILD=debug ARGS='test/sql/write/*'`.
Expected: `All tests passed`.

- [ ] **Step 6: README**

In the Writing section, replace the unsupported bullet about "columns ClickHouse converts on read … not supported **yet**" with:
```markdown
- **Types ClickHouse converts:** columns that the extension reads through ClickHouse conversions (`IPv4/6`,
  `(U)Int256`, `Decimal256`, `BFloat16`, `JSON`, geo types) are written the same way in reverse. DuckDB sends the text
  (or float) form and ClickHouse converts it (`INSERT … SELECT … FROM input(…)`), so they accept exactly what reading
  them produces: `'192.168.0.1'`, `'POINT(1 2)'`, a JSON document. Text ClickHouse cannot parse fails the `INSERT`
  with ClickHouse's error.
```

- [ ] **Step 7: Commit**

```bash
git add -A src test README.md
git commit -m "feat: INSERT into server-converted ClickHouse types through input()

<your harness's Co-Authored-By trailer>"
```

---

## Spec coverage (Phase 1, spec §10.1)

| Spec item | Task |
|---|---|
| D2 writable by default, `READ_ONLY` opt-in (§2) | 1 (+2 for `clickhouse_execute`) |
| D5 auto-commit, ROLLBACK warning (§8) | 1 (mechanism), 2 and 3 (tests) |
| `clickhouse_execute` (§7): resolves the database, rejects row-returning statements, clears the cache, marks the write, rejects READ_ONLY | 2 |
| INSERT / INSERT … SELECT / COPY, column subsets, streaming blocks, single-threaded sink, row count (§4) | 3 |
| Value conversion and range errors, NULL into non-Nullable, FixedString, Enum by label, nested types (§4) | 3 |
| AggregateFunction and other unwritable targets rejected (§4) | 3 |
| Server-converted types through `input()`, plus the spike (§4) | 4 |
| `ch_insert_block_size` (§2) | 3 |
| Unsupported write statements until Phases 2/3; MERGE INTO, CREATE INDEX, CREATE VIEW (§2) | 1 |
| Errors name the column; no password in errors (§8) | 1–4 |
| Tests: `make smoke` against real ClickHouse, a `w_*` database per file, checked through DuckDB and `clickhouse_query`, debug build (§9) | 1–4 |
| README (§10.1) | 1–4 |

Deferred to Phases 2 and 3:
- the `ch_default_table_engine` and `ch_mutations_sync` settings;
- DDL, CTAS, UPDATE, DELETE and TRUNCATE;
- recording the `engine` in `ClickhouseTableEntry`.

Deviation from the spec: MATERIALIZED and ALIAS columns are rejected as INSERT targets. The spec did not mention them, but ClickHouse refuses writes to them, so INSERT has to exclude them.
