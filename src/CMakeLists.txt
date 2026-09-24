add_subdirectory(storage)

add_library(
  clickhouse_ext OBJECT
  clickhouse_connection.cpp
  clickhouse_connection_config.cpp
  clickhouse_conversion.cpp
  clickhouse_error_codes.cpp
  clickhouse_execute.cpp
  clickhouse_filter_pushdown.cpp
  clickhouse_query.cpp
  clickhouse_scanner.cpp
  clickhouse_scanner_extension.cpp
  clickhouse_secrets.cpp
  clickhouse_type_mapping_function.cpp
  clickhouse_types.cpp
  clickhouse_utils.cpp)

# DuckDB's top-level CMakeLists.txt sets CMAKE_CXX_STANDARD to 11 as a CACHE
# variable before this extension is add_subdirectory()-ed in, so the CACHE
# STRING set above is a no-op (set(... CACHE ...) doesn't overwrite an existing
# cache entry). clickhouse-cpp pulls in abseil headers that need C++17 (e.g.
# std::conditional_t and friends), so we request the standard directly on our
# own target instead of forcing the shared cache variable, which would also
# silently affect the icu/json extensions that are configured after this one in
# extension_config.cmake.
target_compile_features(clickhouse_ext PUBLIC cxx_std_17)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:clickhouse_ext>
    PARENT_SCOPE)
