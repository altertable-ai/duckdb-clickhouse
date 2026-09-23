add_subdirectory(storage)

add_library(clickhouse_ext OBJECT clickhouse_scanner_extension.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:clickhouse_ext>
    PARENT_SCOPE)
