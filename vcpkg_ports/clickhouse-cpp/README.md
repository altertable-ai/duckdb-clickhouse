# clickhouse-cpp overlay port

Adapted from [pixonic/duckdb-clickhouse](https://github.com/pixonic/duckdb-clickhouse) (MIT License,
commit 42d842e). Builds clickhouse-cpp 2.6.2 against vcpkg's abseil, cityhash, lz4 and zstd, without -Werror.
We enable the `openssl` feature for TLS (ClickHouse Cloud, port 9440).
