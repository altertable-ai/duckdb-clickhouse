# This file is included by DuckDB's build system. It specifies which extension to load

duckdb_extension_load(clickhouse_scanner
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# used by the tests (TimeZone setting, JSON functions)
duckdb_extension_load(icu)
duckdb_extension_load(json)
