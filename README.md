# DuckDB ClickHouse extension (`clickhouse_scanner`)

Query [ClickHouse](https://clickhouse.com) from DuckDB. The extension attaches a ClickHouse service, including
ClickHouse Cloud, as a DuckDB database you can query and write to. It speaks the native protocol (with TLS) and
pushes projections, filters and `LIMIT` / `ORDER BY … LIMIT` down into ClickHouse.

```sql
INSTALL clickhouse_scanner FROM community;
LOAD clickhouse_scanner;

CREATE SECRET ch (TYPE clickhouse, HOST 'abc123.eu-west-1.aws.clickhouse.cloud', PORT 9440,
                  USER 'default', PASSWORD '...', DATABASE 'analytics');
ATTACH '' AS ch (TYPE clickhouse, SECRET ch);

SELECT event, count(*) FROM ch.analytics.events WHERE ts > now() - INTERVAL 1 DAY GROUP BY ALL;
```

## Connecting

`ATTACH '<connection>' AS name (TYPE clickhouse [, SECRET name] [, SETTINGS 'k=v,…'] [, SHOW_SYSTEM true])`

`<connection>` is either `key=value` pairs (`host=localhost port=9000 user=default password='p w'`) or a URI
(`clickhouse://user:password@host:9000/database`, or `clickhouses://…` for TLS). Settings are applied in this
order: the secret (or the unnamed `clickhouse` secret), then the connection string, then the ATTACH `SETTINGS`
option.

| Option        | Default                    | Description                                               |
|---------------|----------------------------|-------------------------------------------------------------|
| `host`        | `localhost`                | Server host name                                           |
| `port`        | `9000`, or `9440` if secure | Native protocol port                                       |
| `user`        | `default`                  | User name                                                   |
| `password`    |                            | Password (never shown in errors or `duckdb_databases()`)    |
| `database`    | `default`                  | Default schema of the attached database                     |
| `secure`      | `true` when port is 9440   | Use TLS                                                      |
| `ca_cert`     | system CA bundle           | PEM file with the CA certificates to trust                   |
| `skip_verify` | `false`                    | Do not verify the server certificate (testing only)          |
| `compression` | `lz4`                      | `lz4`, `zstd` or `none`                                       |
| `settings`    |                            | ClickHouse settings sent with every query: `k1=v1,k2=v2`      |

`username`, `dbname` and `hostname` are accepted as aliases for `user`, `database` and `host`.

Each ClickHouse database becomes a DuckDB schema. `system`, `INFORMATION_SCHEMA` and `information_schema` are hidden
unless you set `SHOW_SYSTEM true`. Metadata is cached; `CALL clickhouse_clear_cache()` refreshes it. `ATTACH`
connects eagerly, so a bad host or bad credentials fail at `ATTACH` time rather than on the first query.

On Windows there is no system CA bundle that OpenSSL can read, so pass `ca_cert` when using TLS.

## Functions

| Function | Description |
|---|---|
| `clickhouse_query(database, sql)` | Runs any ClickHouse query through an attached database. Projections, filters and LIMIT are pushed into it. |
| `clickhouse_execute(database, sql)` | Runs a ClickHouse statement that returns no rows (DDL, `ALTER`, mutations, `OPTIMIZE`, `SYSTEM …`) and clears that database's metadata cache. Returns `Success = true`. |
| `clickhouse_scan(connection, database, table [, secret := name])` | Reads one table without `ATTACH`. `connection` accepts the same `key=value`/URI forms as `ATTACH`. |
| `clickhouse_clear_cache()` | Forgets cached databases, tables and columns. |
| `clickhouse_type_mapping(type)` | Shows how a ClickHouse type is mapped and read. |

## Settings

| Setting | Default | Description |
|---|---|---|
| `ch_filter_pushdown` | `true` | Push filters into ClickHouse queries |
| `ch_order_pushdown` | `true` | Push `LIMIT` and `ORDER BY … LIMIT` into ClickHouse queries |
| `ch_insert_block_size` | `65536` | Minimum rows per block sent to ClickHouse during `INSERT` (rounded up to whole chunks) |
| `ch_default_table_engine` | `MergeTree` | Table engine used by `CREATE TABLE` |
| `ch_connect_timeout_ms` | `10000` | Connection timeout |
| `ch_receive_timeout_ms` | `300000` | Socket receive timeout |
| `ch_pool_max_connections` | depends on CPU count | Connection pool size per attached database (new ATTACHes) |
| `ch_pool_acquire_mode` | `force` | `force`, `wait` or `try` when the pool is exhausted |
| `ch_pool_wait_timeout_millis` | `30000` | Wait limit for `wait` mode |
| `ch_pool_idle_timeout_millis` | `60000` | Idle connections are closed after this long |
| `ch_debug_show_queries` | `false` | Print every query sent to ClickHouse |

`ch_order_pushdown` only has an effect when `ch_filter_pushdown` is also on: `LIMIT` / `ORDER BY … LIMIT` are pushed
only when every filter on the scan was pushed into ClickHouse too, so turning off filter pushdown also disables
`ORDER BY … LIMIT` pushdown. `EXPLAIN ANALYZE` shows the exact query sent to ClickHouse (`ClickHouse Query`); plain
`EXPLAIN` shows the table and the pushed `ORDER BY`/`LIMIT`.

## Type mapping

| ClickHouse | DuckDB |
|---|---|
| `Bool`, `(U)Int8…64`, `Int128`, `UInt128`, `Float32/64`, `BFloat16` | matching integer / `HUGEINT` / `UHUGEINT` / `FLOAT` / `DOUBLE` |
| `Decimal(P ≤ 38, S)` | `DECIMAL(P, S)` |
| `String`, `FixedString` | `VARCHAR` (must be valid UTF-8, or reading fails with a message suggesting `hex()`/`base64Encode()`) |
| `Date`, `Date32` | `DATE` |
| `DateTime`, `DateTime64` | `TIMESTAMP WITH TIME ZONE` (microseconds; precision above 6 is floor-truncated) |
| `Time` | `TIME` |
| `Time64` | `TIME` (precision ≤ 6) or `TIME_NS` (precision > 6) |
| `UUID` | `UUID` |
| `Enum8/16` | `ENUM` |
| `Array`, `Tuple`, `Map` | `LIST`, `STRUCT`, `MAP` (a `Map` with duplicate keys raises an error; read it with `clickhouse_query` and `mapKeys`/`mapValues` instead) |
| `JSON`, `Object`, `Variant`, `Dynamic` | `JSON` |
| `Nullable(T)`, `LowCardinality(T)`, `SimpleAggregateFunction(f, T)` | `T` |
| `IPv4/6`, `(U)Int256`, `Decimal256`, geo types, others | `VARCHAR` (converted by ClickHouse) |
| `AggregateFunction` | not readable; use `clickhouse_query` with `finalizeAggregation` |

`rowid` is always `NULL` for ClickHouse tables: ClickHouse has no row identifier.

## Writing

Attached databases are writable unless attached with `READ_ONLY`, which rejects every write, including
`clickhouse_execute`:

```sql
ATTACH '' AS ch (TYPE clickhouse, SECRET ch, READ_ONLY);
```

**Every write is committed immediately.** ClickHouse has no multi-statement transactions: each write statement is
sent to ClickHouse and committed when it runs. `COMMIT` does nothing, and `ROLLBACK` cannot undo writes that already
reached ClickHouse. A `ROLLBACK` after a ClickHouse write logs a warning, which is visible after
`CALL enable_logging(level = 'warning')` in `duckdb_logs`. DuckDB allows a transaction to write to only one attached
database, so `BEGIN; INSERT INTO local_table …; INSERT INTO ch.db.t …;` fails on the second `INSERT`: run the two
writes in separate transactions.

`INSERT` (with `VALUES` or a `SELECT`) and `COPY … FROM` stream rows into ClickHouse over the native protocol. A
block is flushed once at least `ch_insert_block_size` rows have been appended, so blocks are always whole DuckDB
chunks (up to 2048 rows each) and can be somewhat larger than `ch_insert_block_size`:

```sql
INSERT INTO ch.analytics.events SELECT * FROM read_parquet('events/*.parquet');
COPY ch.analytics.events FROM 'events.csv';
```

- **Defaults:** only the listed columns are sent, so ClickHouse fills the others with their `DEFAULT` expressions.
  `MATERIALIZED` and `ALIAS` columns cannot be inserted into: list the other columns explicitly. The `DEFAULT`
  keyword inside `VALUES` (e.g. `VALUES (1, DEFAULT)`) sends DuckDB's default for the column, which is `NULL`, not
  ClickHouse's `DEFAULT` expression; to get ClickHouse's default, leave the column out of the column list.
- **Out-of-range values:** values ClickHouse cannot store are rejected, never clamped:
  - dates and timestamps outside the range of `Date`, `Date32`, `DateTime` or `DateTime64`;
  - strings longer than a `FixedString`;
  - `NULL` in a non-`Nullable` column, including `Array`, `Tuple` and `Map` columns, which ClickHouse cannot make
    `Nullable`.
- **Precision:** timestamps and times are written at the column's precision, and anything finer is truncated.
- **Types ClickHouse converts:** columns that the extension reads through ClickHouse conversions (`IPv4/6`,
  `(U)Int256`, `Decimal256`, `BFloat16`, `JSON`, geo types) are written the same way in reverse. DuckDB sends the text
  (or float) form and ClickHouse converts it (`INSERT … SELECT … FROM input(…)`), so they accept exactly what reading
  them produces: `'192.168.0.1'`, `'POINT(1 2)'`, a JSON document. Text ClickHouse cannot parse fails the `INSERT`
  with ClickHouse's error.
- **Atomicity:** an `INSERT` is not atomic. If it fails part-way, ClickHouse may already have committed the blocks sent
  so far. On replicated tables, ClickHouse deduplicates identical blocks by default; attach with
  `SETTINGS 'insert_deduplicate=0'` to turn that off.
- **Unsupported:**
  - `RETURNING` and `ON CONFLICT`;
  - columns of type `Object`, `Variant`, `Dynamic`, `AggregateFunction` or `SimpleAggregateFunction` (`Object`,
    `Variant` and `Dynamic` are read as `JSON` but cannot be written), or nested types holding a
    type that ClickHouse converts (e.g. `Array(IPv4)`). Leave these columns out of the column list, or use
    `clickhouse_execute`.

`CREATE TABLE` (with `IF NOT EXISTS` / `OR REPLACE`), `DROP TABLE`, `CREATE SCHEMA` / `DROP SCHEMA` (ClickHouse
databases; a non-empty one needs `CASCADE`) and `ALTER TABLE … ADD COLUMN` / `DROP COLUMN` / `RENAME COLUMN` /
`RENAME TO` work on attached databases:

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
- Other `ALTER TABLE` forms (changing a column's type or default, constraints) are not supported; use
  `clickhouse_execute`. Columns added with `ALTER TABLE … ADD COLUMN` are always `Nullable`.

`clickhouse_execute(database, sql)` runs any ClickHouse statement that returns no rows. Afterwards, that database's
metadata cache is cleared, so the change is visible to DuckDB right away:

```sql
CALL clickhouse_execute('ch', 'CREATE TABLE analytics.daily (d Date, n UInt64) ENGINE = SummingMergeTree ORDER BY d');
```

## Filter and LIMIT pushdown

Filters are pushed into ClickHouse only on exact types, where DuckDB and ClickHouse are guaranteed to agree on the
result: `Bool`, `(U)Int8`–`128`, `Decimal` with precision ≤ 38, `String`, `Date`/`Date32`, `DateTime`/`DateTime64`
with precision ≤ 6, and `Enum` (plus `IS [NOT] NULL` on any of these, and `=`/`!=` label comparisons on
`Enum` columns). Filters on floating-point columns (NaN semantics differ), `UUID` (ordering differs), `FixedString`,
`Time`/`Time64`, `DateTime64` with precision > 6, nested types, and any column ClickHouse itself converts (see the
type table above) are evaluated by DuckDB after the scan. Optional/runtime filters — join-derived dynamic filters,
and non-dense `IN` lists or `OR` chains that DuckDB keeps above the scan — are also always evaluated by DuckDB, never
pushed into ClickHouse.

`LIMIT` and `ORDER BY … LIMIT` (DuckDB's `TOP_N`) are pushed down only when every `ORDER BY` key is one of the
pushable columns above and every filter on the scan was pushed into ClickHouse. A pushed `ORDER BY` forces a
single-threaded scan.

## Limitations

- `UPDATE` and `DELETE` are not supported yet; run them with `clickhouse_execute()`.
  `MERGE INTO`, indexes and `CREATE VIEW` are not supported.
- Attach with `(TYPE clickhouse, READ_ONLY)` to reject every write.
- ClickHouse has no multi-statement transactions. Two scans in one DuckDB transaction may see different data.
- Not available in DuckDB-WASM (the native protocol needs TCP).

## Development

```bash
git submodule update --init --recursive
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja
make release
make test                                  # tests that need no server
make smoke                                 # all tests against a throw-away ClickHouse 25.8 container (Docker)
make smoke ARGS=test/sql/scan/scalars.test # a single test file or glob
make smoke SMOKE_BUILD=debug               # build and test the debug binary instead
CLICKHOUSE_TEST_KEEP=1 make smoke          # keep the container running afterwards, for debugging
```

The clickhouse-cpp vcpkg port is adapted from [pixonic/duckdb-clickhouse](https://github.com/pixonic/duckdb-clickhouse)
(MIT). The design follows [duckdb/duckdb-postgres](https://github.com/duckdb/duckdb-postgres).

## License

MIT
