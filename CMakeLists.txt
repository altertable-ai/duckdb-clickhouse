cmake_minimum_required(VERSION 3.5...3.29)

set(TARGET_NAME clickhouse_scanner)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

# FORCE: DuckDB's top-level CMakeLists.txt already sets this CACHE variable to "11" before our
# extension's CMakeLists.txt is add_subdirectory()-ed in, so a plain `set(... CACHE ...)` here is a
# no-op. We need real C++17 (clickhouse-cpp pulls in abseil headers that use C++14/17 std type-trait
# aliases such as std::conditional_t under our compiler/libc++), so we force the override.
set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# clickhouse-cpp and its dependencies come from vcpkg (see vcpkg.json)
find_package(OpenSSL REQUIRED)
find_path(CLICKHOUSE_CPP_INCLUDE_DIR clickhouse/client.h REQUIRED)
find_library(CLICKHOUSE_CPP_LIB clickhouse-cpp-lib REQUIRED)
find_library(ABSL_INT128_LIB absl_int128 REQUIRED)
find_library(CITYHASH_LIB cityhash REQUIRED)
find_library(ZSTD_LIB NAMES zstd zstd_static REQUIRED)
find_library(LZ4_LIB lz4 REQUIRED)

include_directories(src/include database-connector/src/include ${CLICKHOUSE_CPP_INCLUDE_DIR})

add_subdirectory(src)

build_static_extension(${TARGET_NAME} ${ALL_OBJECT_FILES})
build_loadable_extension(${TARGET_NAME} " " ${ALL_OBJECT_FILES})

set(CLICKHOUSE_LINK_LIBS
    ${CLICKHOUSE_CPP_LIB}
    ${ABSL_INT128_LIB}
    ${CITYHASH_LIB}
    ${ZSTD_LIB}
    ${LZ4_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto)
if(WIN32)
  list(APPEND CLICKHOUSE_LINK_LIBS ws2_32 crypt32)
endif()

target_link_libraries(${EXTENSION_NAME} ${CLICKHOUSE_LINK_LIBS})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${CLICKHOUSE_LINK_LIBS})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
