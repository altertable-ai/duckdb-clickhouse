if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_check_linkage(ONLY_STATIC_LIBRARY)
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ClickHouse/clickhouse-cpp
    REF "v${VERSION}"
    SHA512 3b6d76a541d75e3565b3d196193ac04baa7e99c54fd175deeb5bb143f9192243966c7d82a5c3159760d8b77f9e3d6b88254bce9ee58af53505dc0c5dc6e429a6
    HEAD_REF master
    PATCHES
        fix-deps-and-build-type.patch
        werror.patch
        expose-low-cardinality-columns.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        openssl WITH_OPENSSL
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DWITH_SYSTEM_ABSEIL=ON
        -DWITH_SYSTEM_LZ4=ON
        # IMPORTANT: keep this OFF (the clickhouse-cpp default). ClickHouse's native-protocol block
        # checksums use a specific old CityHash revision ("cityhash102") vendored under contrib/cityhash,
        # not the latest google/cityhash algorithm that vcpkg's system "cityhash" port provides. Linking
        # against the system port produces syntactically valid but wire-INCOMPATIBLE checksums: every
        # compressed block (lz4/zstd; compression=none is unaffected) is then rejected by the server with
        # "DB::Exception: Checksum doesn't match: corrupted data" (error code 271). clickhouse-cpp builds
        # and installs its own vendored cityhash static lib when this is OFF, which is what
        # find_library(CITYHASH_LIB cityhash) in the extension's top-level CMakeLists.txt picks up.
        -DWITH_SYSTEM_CITYHASH=OFF
        -DWITH_SYSTEM_ZSTD=ON
        -DDEBUG_DEPENDENCIES=OFF
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
