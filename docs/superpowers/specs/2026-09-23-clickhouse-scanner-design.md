# clickhouse_scanner — Design

- **Date:** 2026-09-23
- **Status:** Approved (brainstorming), pending implementation plan
- **Goal:** A DuckDB extension that attaches a ClickHouse service as a read-only DuckDB catalog, modelled on [duckdb/duckdb-postgres](https://github.com/duckdb/duckdb-postgres) (`postgres_scanner`), distributed through DuckDB community extensions.

## 1. Context and decisions

| Reference | Role for this project |
|---|---|
| [duckdb/extension-template](https://github.com/duckdb/extension-template) | Scaffold (C++, vcpkg, extension-ci-tools, DuckDB v1.5.x submodule). |
| [duckdb/duckdb-postgres](https://github.com/duckdb/duckdb-postgres) | Target shape: storage extension, catalog/schema/table entries, pool, secrets, `*_query`, `*_scan`, `*_clear_cache`, `pg_*` settings, `database-connector` submodule. |
| [duckdb/database-connector](https://github.com/duckdb/database-connector) | Shared code reused as a submodule: connection pool, `configure_pool` function, query writer, filter pushdown utilities, ORDER BY/LIMIT optimizer. |
| [pixonic/duckdb-clickhouse](https://github.com/pixonic/duckdb-clickhouse) (MIT) | Prior art in the same stack. We borrow its clickhouse-cpp vcpkg overlay port + patches, type-parsing logic and test fixtures, with attribution. We do **not** fork it. |
| [Query-farm/clickhouse-native](https://github.com/Query-farm/clickhouse-native) (`chsql_native`) | Rust, raw-query table function + Native file reader. Reference only. |

Decisions taken:

1. **Scope v1 = read path, done well.** No INSERT/UPDATE/DELETE/DDL, no `clickhouse_execute`.
2. **Fresh repo from extension-template**, mirroring duckdb-postgres layout and naming; borrow from pixonic with attribution.
3. **Target: ClickHouse Cloud over TLS** (native secure port 9440). Self-hosted plain port 9000 works too and is used in CI. No cluster-specific features, no WASM.
4. **Distribution: DuckDB community extensions** as `clickhouse_scanner` (name verified free in the registry on 2026-09-23).
5. **Transport: native TCP protocol via [clickhouse-cpp](https://github.com/ClickHouse/clickhouse-cpp)** (vcpkg overlay port, `openssl` feature). HTTP/Arrow is not implemented; the `ClickhouseConnection`/`ClickhouseResult` boundary keeps it possible later.
6. **DateTime/DateTime64 → TIMESTAMPTZ** everywhere (DateTime64 precision > 6 truncated to microseconds).

## 2. Architecture

### 2.1 Repository layout

```
CMakeLists.txt, Makefile, extension_config.cmake, vcpkg.json
duckdb/                  (submodule, DuckDB v1.5.x)
extension-ci-tools/      (submodule)
database-connector/      (submodule)
vcpkg_ports/clickhouse-cpp/   (overlay port, adapted from pixonic)
src/
  clickhouse_extension.cpp         load entry point, function/setting registration
  clickhouse_connection.cpp        wraps clickhouse::Client; Query() -> ClickhouseResult
  clickhouse_result.cpp            producer thread + bounded block queue + cancellation
  clickhouse_connection_config.cpp connection string / URI / secret parsing
  clickhouse_secrets.cpp           CREATE SECRET (TYPE clickhouse)
  clickhouse_types.cpp             ClickHouse type string -> DuckDB type + read expression
  clickhouse_conversion.cpp        clickhouse::Column slice -> DuckDB Vector
  clickhouse_filter_pushdown.cpp   TableFilter -> ClickHouse SQL literals/predicates
  clickhouse_scanner.cpp           clickhouse_scan table function (bind/init/scan/to_string)
  clickhouse_query.cpp             clickhouse_query table function
  clickhouse_utils.cpp             identifier/literal quoting
  storage/
    clickhouse_storage_extension.cpp  ATTACH handler
    clickhouse_catalog.cpp            Catalog (read-only)
    clickhouse_schema_set.cpp / clickhouse_schema_entry.cpp
    clickhouse_table_set.cpp  / clickhouse_table_entry.cpp
    clickhouse_transaction_manager.cpp / clickhouse_transaction.cpp
    clickhouse_connection_pool.cpp    database-connector pool specialisation
    clickhouse_optimizer.cpp          ORDER BY/LIMIT pushdown hook
    clickhouse_clear_cache.cpp
  include/  (matching headers; storage/ subfolder)
test/sql/{attach,scan,types,pushdown,secrets,pool,query}/*.test
scripts/setup_clickhouse.sql, docker-compose.yml, scripts/certs/ (self-signed, test only)
```

### 2.2 Catalog mapping

- One `ATTACH` = one ClickHouse service.
- ClickHouse **database** → DuckDB **schema**. The connection's `database` (default `default`) is the default schema.
- `system`, `INFORMATION_SCHEMA`, `information_schema` are hidden unless the ATTACH option `SHOW_SYSTEM true` is set.
- Tables and views (all engines, including `View`, `MaterializedView`, `Distributed`, `Dictionary`) come from `system.tables`; columns from `system.columns` (name, type string, position, default kind, comment). Views appear as tables (read-only).
- Metadata is cached per attached catalog: schemas load on first access, tables per schema on first access. `clickhouse_clear_cache()` invalidates all ClickHouse catalogs.
- The catalog is read-only: every create/alter/drop/insert/update/delete path throws `PermissionException("clickhouse_scanner is read-only")`. `ATTACH ... (READ_ONLY)` is accepted and redundant.

### 2.3 User surface

```sql
CREATE SECRET ch (TYPE clickhouse, HOST 'abc.eu-west-1.aws.clickhouse.cloud', PORT 9440,
                  USER 'default', PASSWORD '...', DATABASE 'analytics', SECURE true);
ATTACH '' AS ch (TYPE clickhouse, SECRET ch);
ATTACH 'clickhouse://user:pw@host:9440/analytics?secure=true' AS ch (TYPE clickhouse);
ATTACH 'host=localhost port=9000 user=default database=default' AS ch (TYPE clickhouse);

SELECT * FROM ch.analytics.events WHERE ts > now() - INTERVAL 1 DAY LIMIT 100;
SELECT * FROM clickhouse_query('ch', 'SELECT uniqExact(user_id) AS u FROM events');
SELECT * FROM clickhouse_scan('clickhouse://...', 'analytics', 'events');
CALL clickhouse_clear_cache();
CALL clickhouse_configure_pool(...);   -- from database-connector
```

- Storage extension registered as `clickhouse_scanner` with alias `clickhouse`.
- Connection string accepts either `key=value` pairs (space-separated, single-quote escaping) or a `clickhouse://` / `clickhouses://` URI. Keys: `host`, `port`, `user`, `password`, `database`, `secure`, `ca_cert`, `skip_verify`, `compression`.
- `secure` defaults to `true` when port is 9440 or scheme is `clickhouses://`, otherwise `false`. Default port: 9440 if secure, else 9000.
- ATTACH option `SETTINGS 'k1=v1,k2=v2'` appends `SETTINGS k1=v1, k2=v2` to every generated query (e.g. `max_execution_time`). Keys validated against `^[a-z_][a-z0-9_]*$`; values emitted as quoted literals unless numeric.
- `clickhouse_query(catalog_name, sql)`: runs arbitrary SQL on the attached catalog's pool; result schema from a `DESCRIBE (<sql>)` round-trip at bind time; same type mapping and conversion as scans; read-only is **not** enforced server-side beyond ClickHouse user permissions (documented).
- `clickhouse_scan(conn_str, database, table)`: attach-less scan, own short-lived connection.

### 2.4 Settings

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `ch_debug_show_queries` | BOOLEAN | false | Print every query sent to ClickHouse to stdout. |
| `ch_filter_pushdown` | BOOLEAN | true | Push filters into the generated WHERE clause. |
| `ch_order_pushdown` | BOOLEAN | true | Push ORDER BY + LIMIT (database-connector optimizer). |
| `ch_compression` | VARCHAR | `lz4` | Wire compression: `lz4`, `zstd`, `none`. |
| `ch_connect_timeout_ms` | UBIGINT | 10000 | TCP/TLS connect timeout. |
| `ch_receive_timeout_ms` | UBIGINT | 300000 | Socket receive timeout. |
| `ch_block_queue_size` | UBIGINT | 4 | Blocks buffered between producer thread and DuckDB workers. |
| `ch_pool_*` | — | from database-connector | Pool size, wait timeout, idle timeout, max lifetime. |

### 2.5 Consistency

ClickHouse has no multi-statement snapshot transactions. `ClickhouseTransaction` only holds a pooled connection for catalog reads; BEGIN/COMMIT/ROLLBACK are no-ops. Two scans in one DuckDB transaction may observe different data. Documented in the README.

## 3. Data path

### 3.1 Scan execution

1. **Bind:** resolve table entry, produce DuckDB column types, remember per-column read expressions.
2. **Init global:** build `SELECT <projected read exprs> FROM <db>.<table> [WHERE ...] [ORDER BY ... LIMIT ...] [SETTINGS ...]`, check out a connection, start `ClickhouseResult`.
3. **ClickhouseResult:** a single producer thread calls `client.SelectCancelable(sql, cb)`. `cb` pushes each non-empty `clickhouse::Block` (with an increasing `batch_index`) into a bounded queue of `ch_block_queue_size`; it returns `false` once the cancel flag is set. Exceptions from the producer are captured and rethrown on the consumer side.
4. **Init local / scan:** each DuckDB worker takes a whole block from the queue and converts it into successive `STANDARD_VECTOR_SIZE` slices. `MaxThreads()` = number of DuckDB threads; `GetBatchIndex` returns the block's batch index so order-preserving plans work.
5. **Teardown / cancellation:** global state destructor (LIMIT satisfied, interrupt, error) sets the cancel flag, drains the queue, joins the producer. Connection returns to the pool only if the query finished or cancelled cleanly; otherwise it is discarded.
6. A pushed-down LIMIT forces `MaxThreads() = 1` so the LIMIT is not applied per worker.

Multi-stream parallel scans (e.g. split by `_part`) are out of scope for v1.

### 3.2 Pushdown

- **Projection:** always. Row-id requests emit `NULL`.
- **Filters** (`ch_filter_pushdown`): constant comparisons, IS [NOT] NULL, IN, conjunction AND/OR, optional filters. Only on columns without a read expression (natively mapped). Literal writer:
  - integers/floats/decimals: plain text; booleans `true`/`false`
  - VARCHAR/ENUM: single-quoted with backslash escaping of `\` and `'`
  - DATE: `toDate32('YYYY-MM-DD')`
  - TIMESTAMPTZ: `fromUnixTimestamp64Micro(<micros>, 'UTC')`
  - UUID: `toUUID('...')`
  - TIME/TIME_NS: `toTime64(...)` as in pixonic
  - anything else: filter not pushed (DuckDB evaluates it).
- **ORDER BY + LIMIT** (`ch_order_pushdown`): database-connector `OrderByAndLimitOptimizer` configured with backtick quoting and backslash escaping, table-scan name `clickhouse_scan`.
- **Aggregate pushdown:** out of scope for v1 (use `clickhouse_query`).
- `EXPLAIN` shows the generated ClickHouse SQL (via the table function's `to_string`).

### 3.3 Type mapping

Mapping works on the `system.columns.type` string, parsed recursively. Each mapping yields a DuckDB `LogicalType` and optionally a **read expression** template applied in the generated SELECT, so no column prevents ATTACH.

| ClickHouse | DuckDB | Read expression |
|---|---|---|
| Bool | BOOLEAN | — |
| Int8/16/32/64, UInt8/16/32/64 | TINYINT…BIGINT, UTINYINT…UBIGINT | — |
| Int128 / UInt128 | HUGEINT / UHUGEINT | — |
| Int256 / UInt256 | VARCHAR | `toString(c)` |
| Float32 / Float64 / BFloat16 | FLOAT / DOUBLE / FLOAT | BFloat16: `toFloat32(c)` |
| Decimal(P,S), P ≤ 38 | DECIMAL(P,S) | — |
| Decimal(P,S), P > 38 | VARCHAR | `toString(c)` |
| String, FixedString(N) | VARCHAR (UTF-8 validated) | — |
| Date, Date32 | DATE | — |
| DateTime[(tz)], DateTime64(p[, tz]) | TIMESTAMPTZ (µs; p > 6 truncated) | — |
| Time / Time64(p) | TIME / TIME_NS (p > 6) | — |
| UUID | UUID | — |
| IPv4, IPv6 | VARCHAR | `toString(c)` |
| Point, Ring, LineString, Polygon, MultiPolygon, MultiLineString | VARCHAR (WKT) | `wkt(c)` |
| Enum8/Enum16 | ENUM(labels) | — |
| Nullable(T) | T (nullable) | inherited |
| LowCardinality(T) | T | inherited |
| SimpleAggregateFunction(f, T) | T | inherited |
| Array(T) | LIST(T) | inherited through `arrayMap` if T needs one |
| Tuple(named…) / Tuple(unnamed…) | STRUCT (unnamed fields named `1`,`2`,…) | inherited per element |
| Map(K, V) | MAP(K, V) | inherited per key/value |
| JSON, Object('json') | JSON | `toJSONString(c)` |
| Variant(...), Dynamic | JSON | `toJSONString(c)` |
| AggregateFunction(...) | VARCHAR, marked unsupported | selecting it throws, suggesting `finalizeAggregation` via `clickhouse_query` |
| anything unrecognised | VARCHAR | `toString(c)` |

A container (Array/Tuple/Map) whose element needs a read expression gets one built recursively (`arrayMap(x -> toString(x), c)`, `tuple(...)`, `mapApply(...)`). If that is impossible, the whole column falls back to `toString(c)` → VARCHAR.

Strings that are not valid UTF-8 raise `InvalidInputException` naming the column and suggesting `clickhouse_query` with `hex()`/`base64Encode()`.

## 4. Connections, security, errors

### 4.1 Connection pool

- One database-connector pool per attached catalog holding `ClickhouseConnection` (wrapping `clickhouse::Client`).
- A native connection runs one query at a time, so each concurrent scan checks out its own connection; catalog loads use the transaction's connection.
- On checkout, a connection idle longer than 30 s is `Ping()`ed; failure → reconnect once, then error.
- Pool knobs come from database-connector (`ch_pool_*`, `clickhouse_configure_pool`).

### 4.2 TLS and secrets

- clickhouse-cpp built with `WITH_OPENSSL`. When `secure`: verify against system CA store by default; `ca_cert '<path>'` overrides; `skip_verify true` disables verification (off by default, logs a warning).
- Secret type `clickhouse`, provider `config`; keys: `HOST`, `PORT`, `USER`, `PASSWORD` (redacted), `DATABASE`, `SECURE`, `CA_CERT`, `SKIP_VERIFY`, `SETTINGS`.
- Resolution at ATTACH: start from the named `SECRET` (or the default `clickhouse` secret if one exists and no SECRET is given), then override with keys from the connection string/URI.
- Passwords never appear in errors, `duckdb_databases()`, or `ch_debug_show_queries` output.

### 4.3 Errors

| Situation | Behaviour |
|---|---|
| ClickHouse server exception | `IOException("ClickHouse error <code> (<name>): <message>")`, plus the query when `ch_debug_show_queries` is on. |
| Connect/TLS/auth failure at ATTACH | `IOException` naming host:port and the underlying reason. |
| Write on attached catalog | `PermissionException("clickhouse_scanner is read-only")`. |
| Invalid UTF-8 | See 3.3. |
| Selecting an unsupported column | `NotImplementedException` naming column + type + workaround. |
| Malformed connection string / URI / unknown key | `InvalidInputException` naming the offending key. |

## 5. Testing and CI

- **sqllogictests** under `test/sql/`, grouped by area. Server-dependent files start with `require-env CLICKHOUSE_TEST_SERVER_AVAILABLE`. Secret and connection-string parsing tests run without a server.
- **docker-compose.yml** with a pinned `clickhouse/clickhouse-server` LTS image exposing 9000 (plain) and 9440 (TLS, self-signed cert under `scripts/certs/`, test only). Tests cover both, with `ca_cert` pointing to the test CA.
- **scripts/setup_clickhouse.sql** creates the `test_db` fixtures: one table per type family including NULL and edge values, a nested-types table, a JSON/Variant table, a view, a 10M-row generated table (`numbers()`) for streaming, LIMIT-cancellation and ordering tests.
- **Pushdown tests** assert on `EXPLAIN` output containing the generated SQL.
- **Type tests** assert both `duckdb_columns()` types and actual values.
- **CI:** extension-ci-tools `_extension_distribution.yml` for all community platforms except `wasm_*`; an additional Linux job starts ClickHouse as a service container, runs the setup script and `make test`.
- Community-extensions `description.yml` submission is a follow-up after v1, not part of this repo's v1 deliverable.

## 6. Revisions made while planning (2026-09-23)

API research against the exact versions we pin changed a few details. These supersede the sections above:

1. **Pinned versions.** DuckDB `v1.5.5` (`d8cdaa33`), extension-ci-tools `72e76e99`, database-connector `0a8505f7` (its `v1.5-variegata` branch) — the same pins as duckdb-postgres' `v1.5-variegata` branch. duckdb-postgres `main` targets DuckDB 2.0-dev and is not a valid reference for APIs.
2. **database-connector on v1.5 only provides the connection pool** (plus attached-catalog lookup and a transaction-manager template). It has no filter pushdown, query writer, ORDER BY/LIMIT optimizer or `configure_pool` function. We therefore:
   - implement our own filter pushdown (`clickhouse_filter_pushdown.cpp`) and optimizer (`clickhouse_optimizer.cpp`);
   - drop `clickhouse_configure_pool()` from v1; pool knobs are the `ch_pool_*` settings, applied at ATTACH time;
   - write our own trivial transaction manager (the database-connector template issues `CHECKPOINT` on the remote side).
3. **No producer thread.** clickhouse-cpp 2.6.2 has a pull API (`BeginSelect` / `NextBlock` / `Cancel`). DuckDB workers pull blocks under a mutex and convert them in parallel. `ch_block_queue_size` is removed.
4. **LowCardinality** columns are decoded by asking the server to send plain columns (query setting `low_cardinality_allow_in_native_format = 0`). No clickhouse-cpp LowCardinality code or patch is needed.
5. **Map(K, V)** is read as `arrayZip(mapKeys(c), mapValues(c))`, so on the wire it is `Array(Tuple(K, V))`, which has the same layout as DuckDB `MAP`.
6. **ORDER BY pushdown** is limited to `ORDER BY … LIMIT` (DuckDB `TOP_N`) and plain `LIMIT/OFFSET`, both gated by `ch_order_pushdown`. It is only pushed when every order key is a plain column that supports pushdown and every table filter on the scan was pushed too. A pushed ORDER BY forces a single-threaded scan.
7. **Filter pushdown safety.** A per-column `supports_pushdown_type` callback makes DuckDB evaluate filters itself on columns we cannot translate exactly: floats (NaN semantics differ), UUID (ordering differs), FixedString, Time, nested types and every column with a read expression. Pushable: Bool, (U)Int8–128, Decimal ≤ 38, String, Date/Date32, DateTime/DateTime64, Enum. Decimal constants are sent as `toDecimal128('<v>', <scale>)`.
8. **Settings.** `ch_compression` becomes the connection option `compression`. The final list is `ch_debug_show_queries`, `ch_filter_pushdown`, `ch_order_pushdown`, `ch_connect_timeout_ms`, `ch_receive_timeout_ms`, `ch_pool_max_connections`, `ch_pool_acquire_mode`, `ch_pool_wait_timeout_millis`, `ch_pool_idle_timeout_millis`.
9. **TLS CA discovery.** vcpkg's OpenSSL does not know the OS trust store. When `secure` is on and no `ca_cert` is given, we probe the usual CA bundle paths (`/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`, `/etc/ssl/cert.pem`, …). On Windows, users must pass `ca_cert`.
10. **EXPLAIN.** The generated ClickHouse SQL is shown by `EXPLAIN ANALYZE` (`dynamic_to_string`), because filters are only known at scan initialisation. Plain `EXPLAIN` shows the table and the pushed ORDER BY/LIMIT.
11. **Test-only helper.** A `clickhouse_type_mapping(type)` table function exposes the type mapper, so type mapping can be tested without a server. It is also useful for users debugging a mapping.
12. **ATTACH connects eagerly** (one pooled connection) so bad hosts or credentials fail at ATTACH.

## 7. Out of scope for v1

INSERT/UPDATE/DELETE/DDL, `clickhouse_execute`, aggregate pushdown, multi-stream scans, HTTP/Arrow transport, DuckDB-WASM, cluster awareness, ClickHouse Native file reader, `GEOMETRY` type mapping.
