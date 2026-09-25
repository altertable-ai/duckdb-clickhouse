# DuckDB ClickHouse extension (`clickhouse_scanner`)

Query and write [ClickHouse](https://clickhouse.com) tables from DuckDB. Attach a ClickHouse server (or
ClickHouse Cloud) as a DuckDB database, then read, join and write its tables with plain DuckDB SQL. The extension
uses ClickHouse's native protocol, with TLS, and pushes filters and `LIMIT`s down to ClickHouse.

```sql
INSTALL clickhouse_scanner FROM community;
LOAD clickhouse_scanner;

CREATE SECRET ch (TYPE clickhouse, HOST 'abc123.eu-west-1.aws.clickhouse.cloud', PORT 9440,
                  USER 'default', PASSWORD '...', DATABASE 'analytics');
ATTACH '' AS ch (TYPE clickhouse, SECRET ch);

SELECT event, count(*) FROM ch.analytics.events WHERE ts > now() - INTERVAL 1 DAY GROUP BY ALL;
```

## Connecting

```sql
ATTACH '<connection>' AS ch (TYPE clickhouse [, SECRET name] [, SETTINGS 'k=v,…'] [, SHOW_SYSTEM true] [, SCHEMA 'db'] [, READ_ONLY]);
```

`<connection>` is either `key=value` pairs (`host=localhost port=9000 user=default password='p w'`) or a URI
(`clickhouse://user:password@host:9000/database`, or `clickhouses://…` for TLS). It can be empty when a secret
provides everything. Values in the connection string override the secret's.

| Option        | Default                     | Description                                          |
|---------------|-----------------------------|------------------------------------------------------|
| `host`        | `localhost`                 | Server host name                                     |
| `port`        | `9000`, or `9440` with TLS  | Native protocol port                                 |
| `user`        | `default`                   | User name                                            |
| `password`    |                             | Password (never shown in errors)                     |
| `database`    | `default`                   | Default schema of the attached database              |
| `secure`      | `true` when port is 9440    | Use TLS                                              |
| `ca_cert`     | system CA bundle            | PEM file with the CA certificates to trust (required for TLS on Windows) |
| `skip_verify` | `false`                     | Do not verify the server certificate (testing only)  |
| `compression` | `lz4`                       | `lz4`, `zstd` or `none`                              |
| `settings`    |                             | ClickHouse settings sent with every query: `k1=v1,k2=v2` |

Each ClickHouse database appears as a DuckDB schema; the system databases are hidden unless you pass
`SHOW_SYSTEM true`. `SCHEMA 'analytics'` shows only that database, so `ch.events` means `ch.analytics.events`
(`clickhouse_query` and `clickhouse_execute` still reach every database). `READ_ONLY` rejects every write. Table and column lists are cached: run
`CALL clickhouse_clear_cache()` after changing tables outside DuckDB.

## Reading

Attached tables and views are queried like any DuckDB table, and can be joined with local data:

```sql
SELECT u.plan, count(*)
FROM ch.analytics.events e JOIN local_users u USING (user_id)
GROUP BY ALL;
```

Filters, the selected columns and `LIMIT` / `ORDER BY … LIMIT` are sent to ClickHouse, so only the rows you need
cross the network. Filters on floating-point, `UUID` and a few other columns are evaluated by DuckDB instead,
because ClickHouse would compare them differently. `EXPLAIN ANALYZE` shows the query sent to ClickHouse.

To run ClickHouse SQL directly, for example to use ClickHouse functions or aggregate on the server:

```sql
SELECT * FROM clickhouse_query('ch', 'SELECT event, uniqExact(user_id) AS users FROM analytics.events GROUP BY event');
```

## Writing

```sql
INSERT INTO ch.analytics.events SELECT * FROM read_parquet('events/*.parquet');
COPY ch.analytics.events FROM 'events.csv';

CREATE TABLE ch.analytics.daily (d DATE PRIMARY KEY, n BIGINT);
CREATE TABLE ch.analytics.top_pages AS SELECT page, count(*) AS views FROM ch.analytics.events GROUP BY page;
ALTER TABLE ch.analytics.daily ADD COLUMN source VARCHAR;

UPDATE ch.analytics.daily SET n = n + 1 WHERE d = DATE '2026-01-01';
DELETE FROM ch.analytics.events WHERE ts < DATE '2025-01-01';
```

Supported: `INSERT` and `COPY … FROM`; `CREATE TABLE` (including `AS SELECT`), `DROP TABLE`; `CREATE SCHEMA` /
`DROP SCHEMA` (ClickHouse databases); `ALTER TABLE … ADD`, `DROP` and `RENAME COLUMN`, `RENAME TO`, and
`ALTER COLUMN … SET`/`DROP DEFAULT`, `SET`/`DROP NOT NULL` and `TYPE` (when every value converts exactly); `UPDATE`,
`DELETE` and `TRUNCATE`. For anything else, run ClickHouse SQL with `clickhouse_execute`:

```sql
CALL clickhouse_execute('ch', 'CREATE TABLE analytics.daily_sums (d Date, n UInt64) ENGINE = SummingMergeTree ORDER BY d');
```

Things to know:

- **No transactions.** ClickHouse commits each statement as it runs, so `ROLLBACK` cannot undo it, and an `INSERT`
  that fails part-way may leave the rows sent so far. A DuckDB transaction can only write to one attached database,
  so write to local tables and to ClickHouse in separate transactions.
- **Inserts** fill the columns you leave out with their ClickHouse `DEFAULT`. Values a column cannot hold (a date out
  of range, `NULL` in a non-`Nullable` column) are rejected, never clamped.
- **New tables** use the `MergeTree` engine by default (see `ch_default_table_engine`), ordered by the `PRIMARY KEY`.
  `CREATE TABLE` runs on the node you are connected to only; for a cluster, use `clickhouse_execute` with
  `ON CLUSTER`.
- **`UPDATE` and `DELETE`** become one ClickHouse statement each: an `ALTER TABLE … UPDATE` mutation, or a
  lightweight `DELETE` (MergeTree tables). The `WHERE` and `SET` clauses can use the table's own columns,
  constants, comparisons, `AND`/`OR`/`NOT`, `IN`, `BETWEEN`, arithmetic, `LIKE`, `CASE`, `coalesce`, casts and common
  string functions. Joins, subqueries, `UPDATE … FROM` and other functions are rejected before anything runs. The
  result is the one DuckDB would compute, with a few exceptions where ClickHouse's own rules apply: `NaN`
  comparisons, integer overflow (it wraps instead of raising an error), division by zero (an error in ClickHouse),
  byte-wise `LIKE`, and how strings are parsed into numbers and dates.
- **Mutations run in the background on the server.** `UPDATE`, and `ALTER COLUMN … TYPE` or `… NOT NULL`, wait for
  it to finish (see `ch_mutations_sync`). If it
  times out it keeps running, so check `system.mutations` before retrying. A mutation that fails can block later
  ones on the same table until you remove it with `KILL MUTATION`.
- The row count that `UPDATE` and `DELETE` report is counted just before the statement runs, so it can be off if
  other clients write at the same time.

## Functions

| Function | Description |
|---|---|
| `clickhouse_query(database, sql)` | Runs a ClickHouse query and returns its rows. |
| `clickhouse_execute(database, sql)` | Runs a ClickHouse statement that returns no rows (DDL, `OPTIMIZE`, `SYSTEM …`), then refreshes the cached table list. |
| `clickhouse_scan(connection, database, table [, secret := name])` | Reads one table without `ATTACH`. |
| `clickhouse_clear_cache()` | Forgets the cached databases, tables and columns. |
| `clickhouse_type_mapping(type)` | Shows which DuckDB type a ClickHouse type is read as. |

## Settings

| Setting | Default | Description |
|---|---|---|
| `ch_filter_pushdown` | `true` | Send filters to ClickHouse |
| `ch_order_pushdown` | `true` | Send `LIMIT` and `ORDER BY … LIMIT` to ClickHouse |
| `ch_insert_block_size` | `65536` | Rows per block sent during `INSERT` |
| `ch_default_table_engine` | `MergeTree` | Engine used by `CREATE TABLE` |
| `ch_mutations_sync` | `2` | Whether `UPDATE` and `ALTER COLUMN` type changes wait: 0 = no, 1 = for this replica, 2 = for all replicas |
| `ch_connect_timeout_ms` | `10000` | Connection timeout |
| `ch_receive_timeout_ms` | `300000` | Socket receive timeout |
| `ch_pool_max_connections` | depends on CPU count | Connections kept per attached database |
| `ch_pool_acquire_mode` | `force` | `force`, `wait` or `try` when all connections are busy |
| `ch_pool_wait_timeout_millis` | `30000` | How long `wait` mode waits |
| `ch_pool_idle_timeout_millis` | `60000` | Idle connections are closed after this long |
| `ch_debug_show_queries` | `false` | Print every query sent to ClickHouse |

## Types

| ClickHouse | DuckDB |
|---|---|
| `Bool`, `(U)Int8…128`, `Float32/64`, `BFloat16` | the matching integer, `HUGEINT`/`UHUGEINT`, `FLOAT`/`DOUBLE` |
| `Decimal(P ≤ 38, S)` | `DECIMAL(P, S)` |
| `String`, `FixedString` | `VARCHAR` |
| `Date`, `Date32` | `DATE` |
| `DateTime`, `DateTime64` | `TIMESTAMP WITH TIME ZONE` (microsecond precision) |
| `Time`, `Time64` | `TIME` (`TIME_NS` above microsecond precision) |
| `UUID` | `UUID` |
| `Enum8/16` | `ENUM` |
| `Array`, `Tuple`, `Map` | `LIST`, `STRUCT`, `MAP` |
| `JSON`, `Variant`, `Dynamic` | `JSON` |
| `Nullable(T)`, `LowCardinality(T)` | `T` |
| `IPv4/6`, `(U)Int256`, `Decimal256`, geo types, others | `VARCHAR` |
| `AggregateFunction` | not readable; use `clickhouse_query` with `finalizeAggregation` |

`String` values must be valid UTF-8; read binary data with `clickhouse_query` and `hex()` or `base64Encode()`.
`CREATE TABLE` maps DuckDB types back the other way (`VARCHAR` → `String`, `TIMESTAMP` → `DateTime64(6, 'UTC')`,
`LIST` → `Array`, …).

## Limitations

- No `CREATE VIEW`, indexes, `MERGE INTO`, `RETURNING` or `ON CONFLICT`. ClickHouse views can be read; create them
  with `clickhouse_execute`.
- ClickHouse has no multi-statement transactions: two scans in one DuckDB transaction may see different data.
- Not available in DuckDB-WASM, which cannot open TCP connections.

## Development

```bash
git submodule update --init --recursive
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja
make release
make test                                  # tests that need no server
make smoke                                 # all tests against throw-away ClickHouse 25.8 containers (Docker)
make smoke ARGS=test/sql/scan/scalars.test # a single test file or glob
make smoke SMOKE_BUILD=debug               # test the debug build instead
CLICKHOUSE_TEST_KEEP=1 make smoke          # keep the containers afterwards, for debugging
```

The design and the exact rules for translating `UPDATE` and `DELETE` are in
[docs/superpowers/specs/2026-09-24-clickhouse-writes-design.md](docs/superpowers/specs/2026-09-24-clickhouse-writes-design.md).
The clickhouse-cpp vcpkg port is adapted from [pixonic/duckdb-clickhouse](https://github.com/pixonic/duckdb-clickhouse)
(MIT). The design follows [duckdb/duckdb-postgres](https://github.com/duckdb/duckdb-postgres).

## License

MIT
