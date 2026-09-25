cmake_minimum_required(VERSION 3.5...3.29)

set(TARGET_NAME clickhouse_scanner)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
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

# DuckDB core is C++11 and emits strong definitions of LogicalType's static
# constexpr members. These targets are C++17, so GCC emits ODR-uses as
# STB_GNU_UNIQUE symbols and the loadable extension fails to link with
# "multiple definition of duckdb::LogicalType::*". -fno-gnu-unique emits them
# as weak symbols that resolve to the core definitions. Clang does not use
# GNU unique symbols.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  foreach(clickhouse_target clickhouse_ext clickhouse_ext_storage)
    target_compile_options(${clickhouse_target} PRIVATE -fno-gnu-unique)
  endforeach()
endif()

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
