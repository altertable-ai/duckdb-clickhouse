# clickhouse-cpp overlay port

Adapted from [pixonic/duckdb-clickhouse](https://github.com/pixonic/duckdb-clickhouse) (MIT License,
commit 42d842e). Builds clickhouse-cpp 2.6.2 against vcpkg's abseil, cityhash, lz4 and zstd, without -Werror.
We enable the `openssl` feature for TLS (ClickHouse Cloud, port 9440).

`insert-query-settings.patch` adds a `Client::BeginInsert(const Query&)` overload that sends the query's settings
(and query id) along with `INSERT`, the way `BeginSelect` already does. Without it, `BeginInsert` sent only the
query text, so neither the user's `settings=` nor `low_cardinality_allow_in_native_format=0` reached the server for
an INSERT.
