add_library(
  clickhouse_ext_storage OBJECT
  clickhouse_catalog.cpp
  clickhouse_catalog_set.cpp
  clickhouse_clear_cache.cpp
  clickhouse_connection_pool.cpp
  clickhouse_optimizer.cpp
  clickhouse_schema_entry.cpp
  clickhouse_schema_set.cpp
  clickhouse_storage_extension.cpp
  clickhouse_table_entry.cpp
  clickhouse_table_set.cpp
  clickhouse_transaction.cpp)

# See the matching comment in ../CMakeLists.txt for clickhouse_ext:
# clickhouse-cpp and the database-connector pool headers need C++17.
target_compile_features(clickhouse_ext_storage PUBLIC cxx_std_17)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:clickhouse_ext_storage>
    PARENT_SCOPE)
