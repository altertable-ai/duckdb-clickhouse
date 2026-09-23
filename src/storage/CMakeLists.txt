# When the first storage source is added here (Task 4) and this becomes a real OBJECT library
# (clickhouse_ext_storage), give it target_compile_features(clickhouse_ext_storage PUBLIC cxx_std_17)
# too, for the same reason as clickhouse_ext in ../CMakeLists.txt.
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES}
    PARENT_SCOPE)
