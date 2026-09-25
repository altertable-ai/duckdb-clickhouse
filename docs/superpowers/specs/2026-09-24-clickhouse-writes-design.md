# clickhouse_scanner: write support — Design

- **Date:** 2026-09-24
- **Status:** Approved (brainstorming). Pending the three implementation plans.
- **Goal:** Lift the read-only limitation. Attached ClickHouse databases become writable from DuckDB: INSERT / INSERT … SELECT / COPY, CREATE / DROP / ALTER / CTAS, UPDATE / DELETE, plus a raw `clickhouse_execute()` escape hatch.
- **Builds on:** the read path described in `docs/superpowers/specs/2026-09-23-clickhouse-scanner-design.md` on branch `feature/clickhouse-scanner`. It was imported as `su/init` without its docs. Everything in that spec still holds except the read-only rule, which this spec replaces.

## 1. Decisions

| # | Decision |
|---|---|
| D1 | Scope: all of INSERT/COPY/CTAS, CREATE/DROP (tables, databases), ALTER (add/drop/rename column, rename table), UPDATE/DELETE/TRUNCATE, and `clickhouse_execute()`. |
| D2 | Writable by default, like duckdb-postgres. `ATTACH … (TYPE clickhouse, READ_ONLY)` restores today's read-only behaviour. |
| D3 | DELETE uses ClickHouse lightweight `DELETE FROM … WHERE …`, synchronously. UPDATE uses `ALTER TABLE … UPDATE … WHERE …` with `mutations_sync` (default 2). |
| D4 | CREATE TABLE produces `ENGINE = <ch_default_table_engine>` (default `MergeTree`) and `ORDER BY` the DuckDB PRIMARY KEY columns, else `tuple()`. |
| D5 | Transactions: every write statement is sent and committed immediately. COMMIT is a no-op. ROLLBACK cannot undo ClickHouse writes: it emits a warning, and the README documents it. |
| D6 | UPDATE/DELETE use statement translation (approach A). The whole statement becomes one ClickHouse statement when its shape and expressions are translatable; otherwise it is an error pointing to `clickhouse_execute()`. Key-based two-phase execution was rejected: ClickHouse does not enforce key uniqueness, so it could modify rows that were not selected. |
| D7 | UPDATE/DELETE report the number of affected rows through a `SELECT count()` run just before the statement. This count is not atomic with respect to concurrent writers. |

## 2. User surface

| DuckDB statement | ClickHouse effect |
|---|---|
| `INSERT INTO ch.db.t [(cols)] VALUES … / SELECT …`, `COPY ch.db.t FROM 'file'` | One native `INSERT INTO db.t (cols)` streamed in blocks. Only the listed columns are sent. |
| `CREATE TABLE [IF NOT EXISTS / OR REPLACE] ch.db.t (…)` | `CREATE TABLE … ENGINE = … ORDER BY …` |
| `CREATE TABLE ch.db.t AS SELECT …` | `CREATE TABLE`, then the same insert path |
| `DROP TABLE [IF EXISTS]` / `DROP VIEW [IF EXISTS]` | `DROP TABLE` / `DROP VIEW`, chosen from the table's engine |
| `CREATE SCHEMA ch.db` / `DROP SCHEMA ch.db [CASCADE]` | `CREATE DATABASE` / `DROP DATABASE` (a non-empty database requires CASCADE) |
| `ALTER TABLE ch.db.t ADD COLUMN [IF NOT EXISTS] c T [DEFAULT e]` / `DROP COLUMN [IF EXISTS] c` / `RENAME COLUMN a TO b` / `RENAME TO u` | `ALTER TABLE db.t ADD/DROP/RENAME COLUMN …` / `RENAME TABLE db.t TO db.u` |
| `DELETE FROM ch.db.t WHERE p` | `DELETE FROM db.t WHERE p SETTINGS lightweight_deletes_sync = 2` |
| `DELETE FROM ch.db.t` (no WHERE), `TRUNCATE ch.db.t` | `TRUNCATE TABLE db.t` |
| `UPDATE ch.db.t SET c = e, … WHERE p` | `ALTER TABLE db.t UPDATE c = CAST(e AS T), … WHERE p SETTINGS mutations_sync = <ch_mutations_sync>` |
| `CALL clickhouse_execute('ch', 'sql')` / `SELECT * FROM clickhouse_execute(...)` | Runs the SQL as is. Returns `Success BOOLEAN`. Clears that catalog's cache. |

The following are rejected with a clear exception:
- `ON CONFLICT` / `RETURNING`;
- `MERGE INTO`, `CREATE INDEX` (already guarded);
- `CREATE VIEW`, sequences, temp tables in a ClickHouse schema;
- UNIQUE / CHECK / FOREIGN KEY constraints;
- ALTER variants other than those listed above (type change, SET/DROP DEFAULT, constraints);
- UPDATE/DELETE with joins, subqueries, `UPDATE … FROM`, or untranslatable expressions;
- `clickhouse_execute` with a statement that returns rows (the error points to `clickhouse_query`).

**Settings:**

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `ch_default_table_engine` | VARCHAR | `MergeTree` | Engine for `CREATE TABLE`/CTAS, e.g. `ReplicatedMergeTree` on self-hosted clusters. Validated as an identifier optionally followed by `(…)`. |
| `ch_insert_block_size` | UBIGINT | 65536 | Rows per block sent during INSERT. Must be greater than 0. |
| `ch_mutations_sync` | UBIGINT | 2 | Value sent as `mutations_sync` with UPDATE (0 = async, 1 = wait locally, 2 = wait on all replicas). |

## 3. Architecture

New units follow the existing layout; each has one responsibility:

- `src/clickhouse_writer.cpp` / `.hpp`: converts DuckDB vectors into clickhouse-cpp columns for a given ClickHouse type string. It is the mirror image of `clickhouse_conversion.cpp`.
- `src/clickhouse_ddl_types.cpp` / `.hpp`: the reverse type mapping, DuckDB `LogicalType` (+ nullability) → ClickHouse type string.
- `src/clickhouse_expression.cpp` / `.hpp`: translates DuckDB bound expressions into ClickHouse SQL using an allow-list. It is used by DML (WHERE/SET) and DDL (DEFAULT).
- `src/storage/clickhouse_insert.cpp` / `.hpp`: `ClickhouseInsert` physical sink (INSERT, COPY, CTAS).
- `src/storage/clickhouse_ddl.cpp` / `.hpp`: builds the CREATE/DROP/ALTER SQL and runs it.
- `src/storage/clickhouse_dml.cpp` / `.hpp`: checks and translates DELETE/UPDATE plans. `ClickhouseDmlOperator` is a source that runs the count and then the statement.
- `src/clickhouse_execute.cpp`: the `clickhouse_execute()` table function.
- Changes to `ClickhouseCatalog` (`PlanInsert`, `PlanCreateTableAs`, logical `PlanDelete`/`PlanUpdate`, `CreateSchema`, `DropSchema`), `ClickhouseSchemaEntry` (`CreateTable`, `DropEntry`, `Alter`), `ClickhouseTableEntry` (remember `engine` from `system.tables.engine`), and `ClickhouseTransaction` (records whether it wrote, for the ROLLBACK warning).

Cache invalidation: DDL, CTAS and `clickhouse_execute` invalidate the affected catalog entries through the existing retire-don't-free mechanism (`ClickhouseCatalogSet::ClearEntries`), so plans that are already bound stay safe.

> **As built (Phase 2):**
> - Invalidation is not per schema: every DDL statement, CTAS, `clickhouse_execute` and `clickhouse_clear_cache()` clears the attached database's whole cache (`ClickhouseCatalog::ClearCache()`: every schema entry, with its table set).
> - Retire-don't-free did not exist before Phase 2; it is implemented by `ClickhouseTransactionManager`. `ClickhouseCatalogSet::ClearEntries()` hands the cleared entries to `RetireEntries()`, which stamps the batch with the id the next DuckDB transaction on that database would get. Every transaction gets a strictly increasing id when it starts, and every catalog lookup runs inside the database's transaction (`ClickhouseCatalogSet` starts it before handing out entries, including for `duckdb_tables()`-style scans). A batch is released when a transaction ends and no transaction with an id below its stamp is still active. Ids, active transactions and batches share one mutex. So an entry stays valid until every transaction that was active when it was retired has ended.
> - A bound plan that outlives its transaction (a prepared statement) is rebound before every `EXECUTE`, because the catalog has no catalog version. The scan's bind data also owns its schema entry (`ClickhouseScanBindData::lifetime`, `GetSchemaEntryOwner`).
> - `CREATE TABLE` / CTAS trust the cache: with a cached table of that name and no `OR REPLACE`, `CREATE TABLE` fails with "already exists … run CALL clickhouse_clear_cache()" (and `IF NOT EXISTS` sends nothing) instead of sending a `CREATE` that could silently leave an empty table where DuckDB dropped the CTAS query.

## 4. INSERT / COPY / CTAS

- `ClickhouseCatalog::PlanInsert` returns `ClickhouseInsert`. DuckDB has already cast the inserted values to the columns' DuckDB types (the read-path mapping), so the writer only converts along known pairs.
- **Column subsets:** only the statement's target columns are sent (`INSERT INTO db.t (a, c)`), so ClickHouse fills the rest with its own DEFAULT/MATERIALIZED values. The column mapping comes from `LogicalInsert::column_index_map`.
- **Streaming:** each sink takes one pooled connection and runs `BeginInsert("INSERT INTO db.t (cols) VALUES")`. Rows accumulate into a block, which is sent every `ch_insert_block_size` rows; `EndInsert` runs at finalize. The sink is single-threaded (`ParallelSink() == false`) and returns the inserted row count. On any error the insert is cancelled and the connection is discarded (`Invalidate`).
- **Atomicity:** each block is atomic; a failure part-way can leave earlier blocks committed (documented). ClickHouse block deduplication on Replicated/Shared tables is left at its server default; the README says how to disable it (`settings=insert_deduplicate=0`).
- **Value conversion** (`clickhouse_writer`):
  - ints, floats, BOOLEAN, HUGEINT/UHUGEINT, DECIMAL(P ≤ 38) are written directly.
  - VARCHAR → String. VARCHAR → FixedString(N) must be at most N bytes and is zero-padded; longer values are a `ConversionException`.
  - DATE → Date/Date32; a value outside the target range (1970-01-01..2149-06-06 / 1900-01-01..2299-12-31) is a `ConversionException`, never clamped.
  - TIMESTAMPTZ → DateTime (seconds, range checked) / DateTime64(p) (ticks at precision p, range checked).
  - TIME/TIME_NS → Time/Time64(p).
  - UUID. ENUM → Enum8/16 by label; a missing label is an error.
  - LIST/STRUCT/MAP → Array/Tuple/Map, recursively. Nullable and LowCardinality wrappers are honoured; NULL into a non-Nullable column is an error.
  - AggregateFunction target columns are an error.
  - Types the read path converts on the server (IPv4/IPv6, (U)Int256, Decimal P > 38, JSON/Object/Variant/Dynamic, geo types) are written as String and converted by the server: `INSERT INTO db.t (cols) SELECT <conversion>(c), … FROM input('c String, …')`. As built: into a Nullable column the conversion is `if(isNull(c), NULL, CAST(assumeNotNull(c) AS T))`, because `CAST(c AS Nullable(T))` turns text that does not parse into NULL; such text fails the INSERT. **Plan-time spike:** check that clickhouse-cpp's `BeginInsert` works with `input()`. If it does not, INSERT into those column types is rejected with an error pointing to `clickhouse_execute`, and the README says so.
- **CTAS:** `PlanCreateTableAs` creates the table (Section 5), then plans `ClickhouseInsert` on the new entry.

  > **As built (Phase 2):** `PlanCreateTableAs` only validates (the CREATE statement and every column's write mode) and plans a `ClickhouseInsert` that owns the `CreateTableInfo`. The sink creates the table when the statement runs: on the first row, or in Finalize when the query yields none. `IF NOT EXISTS` checks for the table on a freshly cleared cache first, and the created table's columns are verified before anything is written. DuckDB plans a plain `PhysicalCreateTable` (the query is dropped) when its cached catalog already has the table and there is no `OR REPLACE`; see Section 3's note.
- **COPY:** `COPY ch.db.t FROM 'file'` goes through DuckDB's insert planning and needs nothing extra.

## 5. DDL

**Reverse type mapping** (`clickhouse_ddl_types`):

| DuckDB | ClickHouse |
|---|---|
| BOOLEAN | Bool |
| TINYINT … BIGINT / UTINYINT … UBIGINT | Int8 … Int64 / UInt8 … UInt64 |
| HUGEINT / UHUGEINT | Int128 / UInt128 |
| FLOAT / DOUBLE | Float32 / Float64 |
| DECIMAL(p, s) | Decimal(p, s) |
| VARCHAR, BLOB | String |
| DATE | Date32 |
| TIMESTAMP_S | DateTime('UTC') |
| TIMESTAMP_MS | DateTime64(3, 'UTC') |
| TIMESTAMP, TIMESTAMPTZ | DateTime64(6, 'UTC') |
| TIMESTAMP_NS | DateTime64(9, 'UTC') |
| TIME / TIME_NS | Time64(6) / Time64(9) (the CREATE statement carries `enable_time_time64_type = 1`) |
| UUID | UUID |
| ENUM | Enum8 (≤ 127 labels) or Enum16, labels numbered 1…n in DuckDB order |
| LIST(T) / STRUCT / MAP(K, V) | Array(T) / Tuple(name T, …) / Map(K, V) |
| JSON | JSON |
| anything else (INTERVAL, BIT, VARINT, UNION, …) | `NotImplementedException` |

**Nullability:** a nullable DuckDB column becomes `Nullable(T)`, except for Array/Tuple/Map, which ClickHouse cannot make Nullable. Those are created non-Nullable, and inserting a NULL into them is an error. PRIMARY KEY columns are NOT NULL. Naive `TIMESTAMP` reads back as `TIMESTAMPTZ` (UTC); this is documented.

- **CREATE TABLE** builds `CREATE [OR REPLACE] TABLE [IF NOT EXISTS] db.t (col T [DEFAULT e], …) ENGINE = <engine> ORDER BY (<pk cols>) | ORDER BY tuple()`.
  - DEFAULT expressions go through `clickhouse_expression`; an untranslatable default is an error.
  - UNIQUE / CHECK / FOREIGN KEY constraints are rejected.
- **CREATE / DROP SCHEMA:** `CREATE DATABASE [IF NOT EXISTS] db`, `DROP DATABASE [IF EXISTS] db`. Without CASCADE, dropping a database that still has tables is an error, which we check ourselves.
- **DROP TABLE / VIEW:** the engine recorded in the table entry picks `DROP TABLE` or `DROP VIEW`. `DROP VIEW` on a table, or `DROP TABLE` on a view, is rejected, matching DuckDB semantics.
- **ALTER:** add, drop and rename column, and rename table, as in Section 2. Anything else is `NotImplementedException("… use clickhouse_execute()")`.
- After each DDL statement: invalidate the schema's table set (or the schema set, for database DDL).

> **As built (Phase 2):**
> - DEFAULT must fold to a constant (DuckDB's constant folding, excluding `now()`-style functions that are only constant within a query), sent as a ClickHouse literal. There is no `clickhouse_expression` translation of DEFAULT expressions; anything else is rejected with a hint to use `clickhouse_execute()`.
> - `DROP VIEW` is not supported. DuckDB casts view-typed catalog lookups to `ViewCatalogEntry`, so our table entries must never be returned for them. `DROP TABLE` refuses ClickHouse views and dictionaries (engines View, MaterializedView, LiveView, WindowView, Dictionary) and points to `clickhouse_execute()`. `ALTER TABLE` refuses them too.
> - `CREATE SCHEMA` / `DROP SCHEMA` refuse `main`, which stands for the connection's database.
> - ALTER resolves column names case-insensitively against the table's ClickHouse columns, as DuckDB identifiers are. It sends the real ClickHouse name, and refuses an `ADD COLUMN` / `RENAME … TO` that would create a case-insensitive duplicate. A table created outside DuckDB with such duplicates is listed with the first of them, and scanning or inserting into it fails with an explicit error. It does not break the rest of its database.
> - ENUM labels containing `'` or `\` are rejected (clickhouse-cpp 2.6.2 cannot parse them back), as are unnamed STRUCT fields (an unnamed Tuple would read back as a different STRUCT).
> - All invalidation clears the whole catalog cache (see Section 3's note).
> - `ADD COLUMN … DEFAULT` on an engine outside the MergeTree family is refused when the table holds rows: on 25.8 only the MergeTree family computes the default for existing rows; Memory (and the Buffer/Merge proxies) give them NULL or the type's zero, and a later mutation cannot store the column in Memory's old blocks. The Log family, Set, Join and EmbeddedRocksDB refuse `ADD COLUMN` themselves.
> - SET/DROP DEFAULT that has to materialize the column first requires `ch_mutations_sync = 2` on a `Replicated*` engine: with 1 the other replicas could apply the default change before the mutation.

## 6. UPDATE / DELETE

- **Hook:** `ClickhouseCatalog` overrides the logical-level `PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &)` and `PlanUpdate(…, LogicalUpdate &)`. DuckDB calls these before it plans the child (`Catalog::PlanDelete` → `planner.CreatePlan(*op.children[0])`), so the logical child is still intact.
- **Accepted shape:** `LogicalDelete/Update → [LogicalProjection]* → [LogicalFilter] → LogicalGet`, where the `LogicalGet` is a ClickHouse scan of the table being modified. Anything else raises `NotImplementedException("UPDATE/DELETE on ClickHouse tables must filter only the modified table with translatable expressions; use clickhouse_execute() for anything else")`: joins, subqueries, `UPDATE … FROM`, other tables, or `RETURNING`.
- **Predicate:** the AND of every `get.table_filters` entry (via `ClickhouseFilterPushdown::TransformFilter`; each must translate, and optional/dynamic filters are skipped because the real predicate is also present in the LogicalFilter) and every expression in the `LogicalFilter` (via `clickhouse_expression`).
- **Expression allow-list** (`clickhouse_expression`):
  - column references, resolved through projections to base column names;
  - constants (existing literal writer);
  - comparisons, AND/OR/NOT, IS [NOT] NULL, IN (constant list), BETWEEN;
  - `+ - * / // %`;
  - LIKE/ILIKE, `starts_with`, `ends_with`, `contains`, `lower`, `upper`, `length`;
  - `coalesce`, CASE WHEN;
  - CAST to types that have a reverse mapping.

  Anything else makes the statement untranslatable. No part of a predicate is ever dropped.
- **Semantics:** translated DML follows ClickHouse semantics where it differs from DuckDB (NaN comparisons, UUID ordering, integer division by zero). This is documented in the README; SELECT pushdown remains exact-only.

  > **As built (Phase 3):** the accepted divergences are a CLOSED list. Translated DML follows ClickHouse semantics only for:
  > - NaN comparisons;
  > - UUID ordering;
  > - integer division and division by zero (`//` and `/` with an integral result become `intDiv`, truncating toward zero in both; a zero divisor fails in ClickHouse where DuckDB returns NULL);
  > - arithmetic overflow and wrap-around, including out-of-range casts and DECIMAL values beyond the column's declared precision (e.g. 9.95 from a Decimal(3,2) into a Decimal(2,1) stores 10, where DuckDB errors), in WHERE as well as in SET;
  > - LIKE / ILIKE collation;
  > - `lowerUTF8` / `upperUTF8` edge cases (e.g. `upper('ß')`).
  >
  > Every other cast and operator must be exact or rejected:
  > - Casts (written, implicit, and the SET value's cast to its column's DuckDB type) are classified by `ClassifyCast` in `clickhouse_expression.cpp`: FLOAT/DOUBLE → integer is `CAST(roundBankers(x) AS T)` (DuckDB's `std::nearbyint`, half to even); DECIMAL → integer and DECIMAL → DECIMAL with a smaller scale are `CAST(round(x, s) AS T)` (half away from zero in both, verified on ClickHouse 25.8). Every cast from VARCHAR to another type, written or implicit (e.g. the SET value's assignment cast), at any nesting level: on 25.8, `CAST(s AS Nullable(T))` is NULL for a string ClickHouse cannot parse (so UPDATE wrote NULL and DELETE removed rows where DuckDB fails the statement), a non-Nullable target fails the mutation and leaves it stuck, and the parse rules differ anyway (`'20240101'` as a DateTime64 is Unix seconds, `'on'` as a Bool is true, `CAST('1.7091' AS Float64)` is 1.7090999999999998). DuckDB folds string constants (`i = '5'`, `SET d = '2024-01-01'`, `'1.5'::DOUBLE`) into literals of the target type before translation, and those translate; a constant it cannot fold (`SET i = 'abc'`) stays a cast and is rejected. Also rejected: FLOAT/DOUBLE → DECIMAL, timestamp precision reductions, (U)HUGEINT → FLOAT/DOUBLE, DECIMAL wider than 7 (FLOAT) / 15 (DOUBLE) digits → floating point, casts involving BLOB or to JSON, and any pair not listed there are rejected. The SET wrapper `CAST(e AS <column's ClickHouse type>)` converts the column's DuckDB type into its ClickHouse type. It is exact for most types, and for BFloat16 it is the conversion INSERT makes. A column DuckDB reads as text that ClickHouse stores as another type (IPv4/6, (U)Int256, Decimal256, FixedString, geo types, JSON, …) is rejected unless the SET value is a constant: the mutation would parse the text, and `CAST(s AS Nullable(IPv4))` is NULL for a string that does not parse, a non-Nullable target fails the mutation (e.g. `'abc'` into FixedString(2)) and leaves it stuck. A constant is parsed strictly, like INSERT does (`CAST(CAST('1.2.3.4' AS IPv4) AS Nullable(IPv4))`, `readWKTPoint(…)` for geo types; NULL is written as is), and the same expressions run first as `SELECT ignore(…)`, so a value that does not convert fails the statement with ClickHouse's error before the count and the mutation (25.8 also refuses such a constant when validating the ALTER, before creating the mutation). Such types nested in Array/Tuple/Map are rejected. For DateTime and DateTime64(p < 6), where CAST truncates toward zero and INSERT floors (`ScaleTicks`), the value is floored first: `fromUnixTimestamp64Micro(m - positiveModulo(m, 10^(6-p)), 'UTC')` with `m = toUnixTimestamp64Micro(CAST(e AS Nullable(DateTime64(6, 'UTC'))))` (verified on 25.8 for pre-1970 values, exact multiples and NULL). Such types nested in Array/Tuple/Map columns are rejected.
  > - `+ - *` on numbers (and DATE ± integer); FLOAT results are wrapped in `toFloat32(…)` (ClickHouse promotes Float32 arithmetic to Float64; rounding the Float64 result of + - * / of two Float32 values gives the single-precision result). `/` with a FLOAT/DOUBLE result is a division, with an integral result `intDiv`; `//` and `%` translate only with an integral result. Unary minus is `negate(x)`.
  > - TIMESTAMP_NS constants are written with every nanosecond (`fromUnixTimestamp64Nano(ns, 'UTC')`), in DML and in DDL DEFAULTs.
  > - `x [NOT] IN (…)` (a DuckDB IN list, or the MARK join InClauseRewriter makes of 5+ values) is `if(isNull(x), NULL, x [NOT] IN (…))`, so the result is independent of `transform_null_in`. This guard is what protects the statement: the lightweight DELETE and the mutation run with the server's default profile as loaded at startup, and query settings do not override it (verified on 25.8.33: with `transform_null_in = 1` in the default profile at startup, a bare `n NOT IN (1, 5)` updated and deleted the NULL row even with `transform_null_in = 0` in the query's settings; the guarded form did not). Other default-profile settings (e.g. `date_time_input_format`, `cast_keep_nullable`) reach mutations the same way; no translated cast parses a string, so the former has nothing to act on, except the SET constants of text-stored columns, which the SET value check has already parsed with strict settings. The count, an ordinary query, also runs with `transform_null_in = 0` whatever the ATTACH's `settings=` say (query settings override the connection's). It and every other query the extension issues itself (the ALTER COLUMN pre-checks, the SET value check, the NULL check, the INSERT conversions, and the catalog's reads of `system.databases`, `system.tables` and `system.columns`, which `offset`, `limit` or a filter would otherwise cut) also pin what decides which rows a query reads and returns (`ClickhouseDml::SemanticSettings`): `final = 0`, the `read`/`timeout` overflow modes (and their `_leaf` forms), `set_overflow_mode` and `group_by_overflow_mode` at `'throw'`, empty `additional_table_filters` and `additional_result_filter`, `limit = 0`, `offset = 0`, `use_query_cache = 0`, plus `cast_ipv4_ipv6_default_on_conversion_error = 0` and `short_circuit_function_evaluation = 'enable'` for the conversions. Otherwise, e.g., `settings=final=1` hid a NULL from the SET NOT NULL count while the mutation still converted it (verified on 25.8: a stuck mutation and an unreadable table). The count therefore counts stored rows, not the ones FINAL would show.
  > - `DELETE` without WHERE / `TRUNCATE` send `TRUNCATE TABLE` only for engines where it removes the rows (`*MergeTree`, Memory, Log, TinyLog, StripeLog, Set, Join); other engines (e.g. Distributed) are rejected, pointing to `clickhouse_execute()`.
  > - SET of a column ClickHouse cannot store NULL in (anything but `Nullable`, `LowCardinality(Nullable)`, `Variant` and `Dynamic`): a NULL constant raises DuckDB's `ConstraintException` (`NOT NULL constraint failed: t.c`) while planning; any other value that may be NULL (not a constant, nor a column that cannot hold NULL) is counted first, `SELECT countIf(isNull(<value>)), … FROM db.t WHERE p` with `apply_deleted_mask = 0`, and a count above 0 raises the same error. On 25.8 the mutation fails on the first NULL (error 349) and stays stuck, masked (deleted, not purged) rows included; `Array`, `Tuple`, `Map` and `JSON` values are never NULL in ClickHouse (a NULL fails when the statement is analysed).
  > - The shape error names the table: `<UPDATE|DELETE> on ClickHouse table "db"."t" must filter only the modified table with translatable expressions (<reason>); use clickhouse_execute() for anything else`. A scan with a pushed-down ORDER BY / LIMIT is rejected.
- **Statements** are listed in Section 2. `ClickhouseDmlOperator` (a source, `ParallelSource() == false`) runs `SELECT count() FROM db.t WHERE p`, then the statement, and returns the count as DuckDB's affected-row count. It marks the transaction as having written.
- ClickHouse server errors surface in the existing format, e.g. updating a sort-key column, or a lightweight delete on an engine that doesn't support it.
- With `READ_ONLY` attach, DuckDB rejects the statement before these hooks run.

## 7. `clickhouse_execute`

`clickhouse_execute(database VARCHAR, sql VARCHAR) → (Success BOOLEAN)`:
- It works like `clickhouse_query`: the attached database is resolved, and anything that isn't a ClickHouse catalog is an error.
- It runs the SQL through the pool with the connection's settings. If the statement returns any rows, it raises an error pointing to `clickhouse_query()`.
- It invalidates the whole catalog cache of that database and marks the transaction as having written.
- A `READ_ONLY` attachment rejects it with the read-only PermissionException.

## 8. Errors

- Every write error names the table (and the column/value where relevant) and never contains the password.
- Conversion failures are `ConversionException`; unsupported statements and shapes are `NotImplementedException` suggesting `clickhouse_execute()`; server errors use `ClickHouse error <code> (<NAME>): <message>`.
- ROLLBACK after a write: `ClickhouseTransactionManager::RollbackTransaction` emits `DUCKDB_LOG_WARNING(context, "ClickHouse writes made in this transaction were already committed and cannot be rolled back (database \"<name>\")")`. It never throws. DuckDB v1.5.4 has no other warning channel for extensions, so the warning only appears when logging is enabled (`CALL enable_logging(level = 'warning')`, visible in `duckdb_logs`). The README therefore states the auto-commit behaviour prominently, and the Phase 1 test checks the log entry.

## 9. Testing

- All write tests run through `make smoke` against a real ClickHouse 25.8. No mocks.
- Each write test file creates and drops its own database (`w_insert`, `w_ddl`, `w_dml`, …), so the shared read fixtures are never mutated and existing counts (e.g. 13 tables in `test_db`) stay valid.
- Every write is verified twice: read back through DuckDB, and independently through `clickhouse_query` (ClickHouse's own view, e.g. `SHOW CREATE TABLE`, `SELECT … FROM system.columns`).
- The existing read-only tests move to `ATTACH … (READ_ONLY)`, and a writable counterpart is added.
- **Phase 1:**
  - INSERT round-trips for every mappable type;
  - column-subset inserts with ClickHouse defaults;
  - multi-block inserts (more than `ch_insert_block_size` rows);
  - INSERT … SELECT from a DuckDB table;
  - `COPY … FROM` a CSV;
  - conversion errors (out-of-range dates, FixedString overflow, NULL into non-Nullable);
  - `clickhouse_execute` (incl. cache invalidation and the error when rows are returned);
  - the ROLLBACK warning;
  - `READ_ONLY` rejecting every write;
  - the `input()` spike outcome.
- **Phase 2:**
  - CREATE/DROP TABLE and SCHEMA with IF [NOT] EXISTS, OR REPLACE, CASCADE;
  - CTAS;
  - each ALTER variant;
  - `ch_default_table_engine`;
  - rejected constraints and CREATE VIEW;
  - DROP VIEW vs DROP TABLE;
  - generated DDL checked via `SHOW CREATE TABLE`.
- **Phase 3:**
  - DELETE/UPDATE with translatable predicates and SET expressions, checking the affected-row counts;
  - TRUNCATE and DELETE without WHERE;
  - rejections (joins, subqueries, `UPDATE … FROM`, untranslatable functions, `RETURNING`);
  - the server error on sort-key update;
  - a regression test that an untranslatable predicate never results in a partial statement.
- Each phase's tests also run under the debug build (`make smoke SMOKE_BUILD=debug`).

## 10. Phasing

One spec and three plans, each shippable and executed with subagent-driven development on branch `su/init`:

1. **Write foundation:** read-write by default, the READ_ONLY opt-in, transaction write-tracking and the ROLLBACK warning, `clickhouse_writer`, `ClickhouseInsert` (INSERT/COPY), `clickhouse_execute`, the settings, the README update.
2. **DDL:** reverse type mapping, CREATE/DROP TABLE and SCHEMA, CTAS, ALTER, `ch_default_table_engine`, engine-aware DROP.
3. **DML:** `clickhouse_expression`, DELETE/UPDATE/TRUNCATE translation, `ch_mutations_sync`.

## 11. Out of scope

- `ON CONFLICT` / upserts, `RETURNING`, `MERGE INTO`;
- CREATE VIEW / materialized views from DuckDB SQL;
- CREATE INDEX, sequences;
- `ON CLUSTER` DDL;
- parallel (multi-connection) INSERT;
- buffering writes until COMMIT;
- key-based UPDATE/DELETE fallback;
- lightweight `UPDATE` (patch parts).
