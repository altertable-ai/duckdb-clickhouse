# clickhouse_scanner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A DuckDB extension (`clickhouse_scanner`) that attaches a ClickHouse service as a read-only DuckDB catalog, like duckdb-postgres does for Postgres.

**Architecture:** A C++ extension built from duckdb/extension-template. A storage extension (`ATTACH … (TYPE clickhouse)`) exposes ClickHouse databases as DuckDB schemas. Scans generate ClickHouse SQL with projection, filter and LIMIT/TOP-N pushdown. The native TCP protocol is spoken through clickhouse-cpp (vcpkg), with pooled connections from duckdb/database-connector. ClickHouse types DuckDB cannot decode natively are cast on the server through per-column read expressions.

**Tech Stack:** C++17, DuckDB v1.5.5 extension API, clickhouse-cpp 2.6.2 (+OpenSSL) via a vcpkg overlay port, duckdb/database-connector (pool only), sqllogictest, Docker (`clickhouse/clickhouse-server:25.8`), GitHub Actions (extension-ci-tools `v1.5-variegata`).

**Spec:** `docs/superpowers/specs/2026-09-23-clickhouse-scanner-design.md`. Read it, and especially **section 6 "Revisions made while planning"**, which overrides earlier sections.

## Global Constraints

- Extension name `clickhouse_scanner`. Storage extension registered as both `clickhouse_scanner` and `clickhouse`. Catalog type string `"clickhouse"`.
- DuckDB submodule pinned to `v1.5.5` = `d8cdaa33fda8df955cc76ef58a280f68f4cd43fa`. extension-ci-tools pinned to `72e76e99cd7fee45a99739cd118ec2db64e034ec`. database-connector pinned to `0a8505f775dae7bb30edd37f848d5975bf72884e`.
- clickhouse-cpp `2.6.2` with the `openssl` feature.
- Read-only: every write path throws `PermissionException("clickhouse_scanner is read-only: writing to ClickHouse is not supported")`.
- Settings prefix `ch_`. The only settings are: `ch_debug_show_queries`, `ch_filter_pushdown`, `ch_order_pushdown`, `ch_connect_timeout_ms`, `ch_receive_timeout_ms`, `ch_pool_max_connections`, `ch_pool_acquire_mode`, `ch_pool_wait_timeout_millis`, `ch_pool_idle_timeout_millis`.
- Connection options (connection string, URI query, secret keys): `host`, `port`, `user`, `password`, `database`, `secure`, `ca_cert`, `skip_verify`, `compression`, `settings`. Aliases: `username`→`user`, `dbname`→`database`, `hostname`→`host`.
- DateTime / DateTime64 → `TIMESTAMP WITH TIME ZONE` (µs; precision > 6 floor-truncated).
- Passwords never appear in errors, `duckdb_databases().path` or `duckdb_secrets()`.
- Code style: DuckDB conventions. Tabs, `namespace duckdb`, `unique_ptr`/`make_uniq`, `StringUtil`, and DuckDB exception types (`IOException`, `BinderException`, `InvalidInputException`, `PermissionException`, `NotImplementedException`, `InternalException`, `ConversionException`).
- Server-dependent tests start with `require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD`, so `make test` without a server skips them.
- Commits end with the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Build & test cheat sheet (used by every task)

```bash
# one-time
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
export GEN=ninja
# build (first build compiles DuckDB: 10-20 min; later builds are incremental)
make release
# tests that need no server
./build/release/test/unittest "test/*"
# tests against a throw-away ClickHouse container (Task 2 onward): all tests, one file, or a glob
make smoke
make smoke ARGS=test/sql/scan/scalars.test
make smoke ARGS='test/sql/scan/*'
make smoke SMOKE_BUILD=debug ARGS='test/sql/scan/*'
```

Troubleshooting:
- **vcpkg ports fail under CMake ≥ 4.0** with "Compatibility with CMake < 3.5 has been removed". Add `"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"` to the `OPTIONS` of `vcpkg_cmake_configure` in `vcpkg_ports/clickhouse-cpp/portfile.cmake`, and export `CMAKE_POLICY_VERSION_MINIMUM=3.5` before `make`.
- **Debugging a failing smoke run**: `CLICKHOUSE_TEST_KEEP=1 make smoke ARGS=…` keeps the container (`clickhouse-scanner-test`) running afterwards; inspect it with `docker exec -it clickhouse-scanner-test clickhouse-client --user duckdb --password duckdb`, then `docker rm -f clickhouse-scanner-test`.

## File Structure

```
.gitmodules                      duckdb, extension-ci-tools, database-connector submodules
.gitignore
CMakeLists.txt                   extension target, links clickhouse-cpp + OpenSSL
Makefile                         extension-ci-tools include + `smoke` target
extension_config.cmake           loads clickhouse_scanner (+ icu, json for tests)
vcpkg.json                       clickhouse-cpp[openssl], openssl
vcpkg_ports/clickhouse-cpp/      overlay port (adapted from pixonic/duckdb-clickhouse, MIT)
scripts/test_with_clickhouse.sh  `make smoke`: throw-away ClickHouse 25.8 container on random ports, fixtures, env vars, tests, cleanup
scripts/generate_test_certs.sh   self-signed CA + localhost cert → scripts/certs/ (gitignored)
scripts/clickhouse/tls.xml       server config enabling tcp_port_secure 9440
scripts/setup_clickhouse.sql     test fixtures (test_db, other_db)
src/CMakeLists.txt               object library for src/*.cpp
src/storage/CMakeLists.txt       object library for src/storage/*.cpp
src/include/clickhouse_scanner_extension.hpp   Extension class
src/clickhouse_scanner_extension.cpp           entry point; registers functions, settings, storage, optimizer
src/include/clickhouse_utils.hpp / src/clickhouse_utils.cpp            quoting, UTF-8 validation
src/include/clickhouse_types.hpp / src/clickhouse_types.cpp            type-string parser, DuckDB mapping, read expressions
src/include/clickhouse_type_mapping_function.hpp / src/clickhouse_type_mapping_function.cpp   clickhouse_type_mapping()
src/include/clickhouse_connection_config.hpp / src/clickhouse_connection_config.cpp  option/URI/key=value parsing
src/include/clickhouse_secrets.hpp / src/clickhouse_secrets.cpp        CREATE SECRET (TYPE clickhouse)
src/include/clickhouse_connection.hpp / src/clickhouse_connection.cpp  clickhouse::Client wrapper, errors, TLS, debug print
src/include/clickhouse_conversion.hpp / src/clickhouse_conversion.cpp  clickhouse::Column → DuckDB Vector
src/include/clickhouse_filter_pushdown.hpp / src/clickhouse_filter_pushdown.cpp  TableFilter → ClickHouse WHERE
src/include/clickhouse_scanner.hpp / src/clickhouse_scanner.cpp        bind data, scan callbacks, clickhouse_scan()
src/clickhouse_query.cpp                                               clickhouse_query()
src/include/storage/clickhouse_connection_pool.hpp / src/storage/clickhouse_connection_pool.cpp
src/include/storage/clickhouse_catalog.hpp / src/storage/clickhouse_catalog.cpp
src/include/storage/clickhouse_catalog_set.hpp / src/storage/clickhouse_catalog_set.cpp
src/include/storage/clickhouse_schema_set.hpp / src/storage/clickhouse_schema_set.cpp
src/include/storage/clickhouse_schema_entry.hpp / src/storage/clickhouse_schema_entry.cpp
src/include/storage/clickhouse_table_set.hpp / src/storage/clickhouse_table_set.cpp
src/include/storage/clickhouse_table_entry.hpp / src/storage/clickhouse_table_entry.cpp
src/include/storage/clickhouse_transaction.hpp / src/storage/clickhouse_transaction.cpp   Transaction + TransactionManager
src/include/storage/clickhouse_storage_extension.hpp / src/storage/clickhouse_storage_extension.cpp  ATTACH handler
src/include/storage/clickhouse_clear_cache.hpp / src/storage/clickhouse_clear_cache.cpp
src/include/storage/clickhouse_optimizer.hpp / src/storage/clickhouse_optimizer.cpp   LIMIT / TOP_N pushdown
test/sql/extension/*.test  types/*.test  attach/*.test  catalog/*.test  scan/*.test  pushdown/*.test  query/*.test
.github/workflows/MainDistributionPipeline.yml, IntegrationTests.yml
README.md
```

## Task Order and Dependencies

1. Scaffold. Build links clickhouse-cpp.
2. `make smoke`: throw-away ClickHouse container, fixtures and test plumbing.
3. Utils + type parser/mapper + `clickhouse_type_mapping()` (no server).
4. Connection config, secrets, connection, pool, transaction, storage extension: `ATTACH` works.
5. Catalog: schemas, tables, columns, clear cache, read-only errors.
6. Conversion + scan: `SELECT` works for every type, parallel, cancellable.
7. Filter pushdown.
8. LIMIT / TOP-N pushdown optimizer.
9. `clickhouse_query()` and `clickhouse_scan()`.
10. CI workflows + README.

Every task depends on the ones before it.

---

### Task 1: Repository scaffold that links clickhouse-cpp

**Files:**
- Create: `.gitmodules` (through `git submodule add`), `.gitignore`, `CMakeLists.txt`, `Makefile`, `extension_config.cmake`, `vcpkg.json`
- Create: `vcpkg_ports/clickhouse-cpp/{portfile.cmake,vcpkg.json,fix-deps-and-build-type.patch,werror.patch,expose-low-cardinality-columns.patch,README.md}`
- Create: `src/CMakeLists.txt`, `src/storage/CMakeLists.txt`, `src/include/clickhouse_scanner_extension.hpp`, `src/clickhouse_scanner_extension.cpp`
- Test: `test/sql/extension/load.test`

**Interfaces:**
- Produces: `LoadInternal(ExtensionLoader &loader)` in `src/clickhouse_scanner_extension.cpp`, where later tasks register functions, settings and extensions. Also CMake object libraries `clickhouse_ext` (in `src/CMakeLists.txt`) and `clickhouse_ext_storage` (in `src/storage/CMakeLists.txt`); later tasks append `.cpp` files to them. Also SQL function `clickhouse_client_version() -> VARCHAR`.

- [ ] **Step 1: Add submodules at the pinned commits**

```bash
cd /Users/redox/dev/altertable-ai/duckdb-clickhouse
git submodule add https://github.com/duckdb/duckdb.git duckdb
git -C duckdb checkout d8cdaa33fda8df955cc76ef58a280f68f4cd43fa
git submodule add https://github.com/duckdb/extension-ci-tools.git extension-ci-tools
git -C extension-ci-tools checkout 72e76e99cd7fee45a99739cd118ec2db64e034ec
git submodule add https://github.com/duckdb/database-connector.git database-connector
git -C database-connector checkout 0a8505f775dae7bb30edd37f848d5975bf72884e
git submodule status
```
Expected: three lines showing exactly those three hashes.

- [ ] **Step 2: Copy the clickhouse-cpp overlay port from pixonic/duckdb-clickhouse (MIT) at commit 42d842e**

```bash
mkdir -p vcpkg_ports/clickhouse-cpp
for f in portfile.cmake vcpkg.json fix-deps-and-build-type.patch werror.patch expose-low-cardinality-columns.patch; do
  curl -fsSL "https://raw.githubusercontent.com/pixonic/duckdb-clickhouse/42d842e/vcpkg_ports/clickhouse-cpp/$f" -o "vcpkg_ports/clickhouse-cpp/$f"
done
grep -n 'openssl WITH_OPENSSL' vcpkg_ports/clickhouse-cpp/portfile.cmake
```
Expected: the grep prints one line, meaning the port supports the `openssl` feature. The low-cardinality patch is kept only because the portfile lists it. We don't depend on it.

Create `vcpkg_ports/clickhouse-cpp/README.md`:
```markdown
# clickhouse-cpp overlay port

Adapted from [pixonic/duckdb-clickhouse](https://github.com/pixonic/duckdb-clickhouse) (MIT License,
commit 42d842e). Builds clickhouse-cpp 2.6.2 against vcpkg's abseil, cityhash, lz4 and zstd, without -Werror.
We enable the `openssl` feature for TLS (ClickHouse Cloud, port 9440).
```

- [ ] **Step 3: Write the build files**

`vcpkg.json`:
```json
{
  "dependencies": [
    {
      "name": "clickhouse-cpp",
      "features": ["openssl"]
    },
    "openssl"
  ],
  "vcpkg-configuration": {
    "overlay-ports": [
      "./vcpkg_ports",
      "./extension-ci-tools/vcpkg_ports"
    ]
  }
}
```

`extension_config.cmake`:
```cmake
# This file is included by DuckDB's build system. It specifies which extension to load

duckdb_extension_load(clickhouse_scanner
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# used by the tests (TimeZone setting, JSON functions)
duckdb_extension_load(icu)
duckdb_extension_load(json)
```

`Makefile`:
```make
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=clickhouse_scanner
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
```

`CMakeLists.txt`:
```cmake
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
```

`src/CMakeLists.txt`:
```cmake
add_subdirectory(storage)

add_library(clickhouse_ext OBJECT clickhouse_scanner_extension.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:clickhouse_ext>
    PARENT_SCOPE)
```

`src/storage/CMakeLists.txt`. It starts with no sources. CMake rejects an empty OBJECT library, so the first storage file is added in Task 4. Until then the file only forwards the list:
```cmake
set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES}
    PARENT_SCOPE)
```

`.gitignore`:
```
build
.idea
cmake-build-debug
duckdb_unittest_tempdir/
.DS_Store
testext
scripts/certs/
vcpkg_installed/
```

- [ ] **Step 4: Write the failing test**

`test/sql/extension/load.test`:
```
# name: test/sql/extension/load.test
# description: the extension loads and links clickhouse-cpp
# group: [extension]

statement error
SELECT clickhouse_client_version();
----
Catalog Error: Scalar Function with name clickhouse_client_version does not exist!

require clickhouse_scanner

query I
SELECT clickhouse_client_version() LIKE '2.6.%';
----
true
```

- [ ] **Step 5: Write the extension entry point**

`src/include/clickhouse_scanner_extension.hpp`:
```cpp
#pragma once

#include "duckdb.hpp"

namespace duckdb {

class ClickhouseScannerExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};

} // namespace duckdb
```

`src/clickhouse_scanner_extension.cpp`:
```cpp
#define DUCKDB_EXTENSION_MAIN

#include "clickhouse_scanner_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <clickhouse/client.h>

namespace duckdb {

static void ClickhouseClientVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto version = clickhouse::Client::GetVersion();
	auto text = StringUtil::Format("%d.%d.%d", version.major, version.minor, version.patch);
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, text);
}

static void LoadInternal(ExtensionLoader &loader) {
	ScalarFunction version_function("clickhouse_client_version", {}, LogicalType::VARCHAR,
	                                ClickhouseClientVersionFunction);
	loader.RegisterFunction(version_function);
}

void ClickhouseScannerExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string ClickhouseScannerExtension::Name() {
	return "clickhouse_scanner";
}

std::string ClickhouseScannerExtension::Version() const {
#ifdef EXT_VERSION_CLICKHOUSE_SCANNER
	return EXT_VERSION_CLICKHOUSE_SCANNER;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(clickhouse_scanner, loader) {
	duckdb::LoadInternal(loader);
}
}
```

- [ ] **Step 6: Build and run the test**

```bash
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja
make release
./build/release/test/unittest test/sql/extension/load.test
```
Expected: `All tests passed (…)`. If vcpkg fails on CMake 4, apply the troubleshooting note from the cheat sheet. If the linker reports undefined `absl`/`cityhash`/`LZ4`/`ZSTD` symbols, check `build/release/vcpkg_installed/*/lib` for the real library file names and adjust the `find_library` names.

- [ ] **Step 7: Commit**

```bash
git add .gitmodules duckdb extension-ci-tools database-connector .gitignore CMakeLists.txt Makefile \
  extension_config.cmake vcpkg.json vcpkg_ports src test
git commit -m "build: scaffold clickhouse_scanner extension linking clickhouse-cpp

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `make smoke`: throw-away ClickHouse container and fixtures

This mirrors `make smoke` in duckdb-altertable (`scripts/test_with_mock.sh`). A script starts a disposable container on random localhost ports, waits for it, loads fixtures, exports `CLICKHOUSE_TEST_*` variables for `require-env`, runs the tests and always removes the container. CI runs the same script (Task 10). A GitHub service container can't be used because the TLS config and certificates must be mounted from the checkout.

**Files:**
- Create: `scripts/test_with_clickhouse.sh`, `scripts/generate_test_certs.sh`, `scripts/clickhouse/tls.xml`, `scripts/setup_clickhouse.sql`
- Modify: `Makefile` (append the `smoke` target)
- Test: `test/sql/smoke/environment.test`

**Interfaces:**
- Produces:
  - `make smoke [ARGS=<unittest args>] [SMOKE_BUILD=release|debug]`. With no `ARGS` it runs every test under `test/`.
  - The environment variables `CLICKHOUSE_TEST_HOST` (`127.0.0.1`), `CLICKHOUSE_TEST_PORT` (mapped native port), `CLICKHOUSE_TEST_TLS_PORT` (mapped TLS port), `CLICKHOUSE_TEST_USER` (`duckdb`), `CLICKHOUSE_TEST_PASSWORD` (`duckdb`) and `CLICKHOUSE_TEST_CA_CERT` (absolute path of the test CA).
  - Databases `test_db` and `other_db` with the tables in Step 4.
  - `CLICKHOUSE_TEST_KEEP=1` keeps the container after the run.
- Every later server test attaches with:
  `ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse)`.

- [ ] **Step 1: Write the failing test**

`test/sql/smoke/environment.test` checks the plumbing only. It needs no extension code beyond Task 1:
```
# name: test/sql/smoke/environment.test
# description: make smoke exports the ClickHouse connection variables
# group: [smoke]

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_TLS_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

require-env CLICKHOUSE_TEST_CA_CERT

query IIII
SELECT '${CLICKHOUSE_TEST_HOST}', '${CLICKHOUSE_TEST_PORT}'::INTEGER > 0, '${CLICKHOUSE_TEST_TLS_PORT}'::INTEGER > 0, '${CLICKHOUSE_TEST_PORT}' <> '${CLICKHOUSE_TEST_TLS_PORT}';
----
127.0.0.1	true	true	true
```

Run: `./build/release/test/unittest test/sql/smoke/environment.test`
Expected: the test is **skipped** (`require-env` not satisfied). That is correct for plain `make test`. `make smoke` does not exist yet.

- [ ] **Step 2: Certificates and TLS config**

`scripts/generate_test_certs.sh` (make it executable):
```bash
#!/usr/bin/env bash
# Generates a throw-away CA and a localhost server certificate for the ClickHouse test server.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)/certs"
mkdir -p "$DIR"
cd "$DIR"
if [ -f server.crt ] && [ -f ca.crt ]; then
  exit 0
fi
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=clickhouse-scanner-test-ca" \
  -keyout ca.key -out ca.crt
openssl req -newkey rsa:2048 -nodes -subj "/CN=localhost" -keyout server.key -out server.csr
printf "subjectAltName=DNS:localhost,IP:127.0.0.1\n" > server.ext
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 3650 -sha256 \
  -extfile server.ext -out server.crt
# the server runs as uid 101 inside the container and must be able to read the key
chmod 644 server.key ca.key
rm -f server.csr server.ext ca.srl
```

`scripts/clickhouse/tls.xml`:
```xml
<clickhouse>
    <tcp_port_secure>9440</tcp_port_secure>
    <openSSL>
        <server>
            <certificateFile>/etc/clickhouse-server/certs/server.crt</certificateFile>
            <privateKeyFile>/etc/clickhouse-server/certs/server.key</privateKeyFile>
            <verificationMode>none</verificationMode>
            <loadDefaultCAFile>false</loadDefaultCAFile>
            <cacheSessions>true</cacheSessions>
            <disableProtocols>sslv2,sslv3</disableProtocols>
            <preferServerCiphers>true</preferServerCiphers>
        </server>
    </openSSL>
</clickhouse>
```

- [ ] **Step 3: The smoke script and Makefile target**

`scripts/test_with_clickhouse.sh` (make it executable):
```bash
#!/usr/bin/env bash
# `make smoke`: run the sqllogictests against a throw-away ClickHouse container.
#
# Starts clickhouse/clickhouse-server on random localhost ports (native + TLS), loads scripts/setup_clickhouse.sql,
# exports the CLICKHOUSE_TEST_* variables read by `require-env` in the tests, runs the tests and removes the container
# on exit, whatever happens. Used unchanged locally and in CI.
#
# Usage: scripts/test_with_clickhouse.sh [unittest arguments...]
#   no arguments  runs every test under test/
#   arguments     are passed to the unittest binary, e.g. test/sql/scan/scalars.test
# Environment:
#   SMOKE_BUILD=release|debug    which build to test (default: release)
#   CLICKHOUSE_IMAGE             image to run (default: clickhouse/clickhouse-server:25.8)
#   CLICKHOUSE_TEST_KEEP=1       keep the container after the run, for debugging
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${ROOT}"

IMAGE="${CLICKHOUSE_IMAGE:-clickhouse/clickhouse-server:25.8}"
CONTAINER_NAME="clickhouse-scanner-test"
BUILD="${SMOKE_BUILD:-release}"
UNITTEST="./build/${BUILD}/test/unittest"
CH_USER="duckdb"
CH_PASSWORD="duckdb"

if ! command -v docker &>/dev/null; then
  echo "ERROR: Docker is not installed or not on PATH." >&2
  exit 1
fi
if [[ ! -x "${UNITTEST}" ]]; then
  echo "ERROR: ${UNITTEST} not found; run 'make ${BUILD}' first." >&2
  exit 1
fi

./scripts/generate_test_certs.sh

# remove a leftover container from an earlier run
docker rm -f "${CONTAINER_NAME}" &>/dev/null || true

echo "==> Starting ${IMAGE} ..."
docker run -d --name "${CONTAINER_NAME}" \
  -p "127.0.0.1::9000" \
  -p "127.0.0.1::9440" \
  -e CLICKHOUSE_USER="${CH_USER}" \
  -e CLICKHOUSE_PASSWORD="${CH_PASSWORD}" \
  -e CLICKHOUSE_DEFAULT_ACCESS_MANAGEMENT=1 \
  -v "${ROOT}/scripts/clickhouse/tls.xml:/etc/clickhouse-server/config.d/tls.xml:ro" \
  -v "${ROOT}/scripts/certs:/etc/clickhouse-server/certs:ro" \
  --ulimit nofile=262144:262144 \
  "${IMAGE}" >/dev/null

cleanup() {
  if [[ "${CLICKHOUSE_TEST_KEEP:-}" == "1" ]]; then
    echo "==> Keeping container ${CONTAINER_NAME} (CLICKHOUSE_TEST_KEEP=1)"
  else
    echo "==> Removing container ${CONTAINER_NAME} ..."
    docker rm -f "${CONTAINER_NAME}" &>/dev/null || true
  fi
}
trap cleanup EXIT

ch_client() {
  docker exec -i "${CONTAINER_NAME}" clickhouse-client --user "${CH_USER}" --password "${CH_PASSWORD}" "$@"
}

echo "==> Waiting for ClickHouse to accept queries ..."
ready=0
for _ in $(seq 1 60); do
  if ch_client --query "SELECT 1" &>/dev/null; then
    ready=1
    break
  fi
  sleep 1
done
if [[ "${ready}" != "1" ]]; then
  echo "ERROR: ClickHouse did not become ready." >&2
  docker logs --tail 50 "${CONTAINER_NAME}" >&2 || true
  exit 1
fi

echo "==> Loading fixtures ..."
ch_client --multiquery < scripts/setup_clickhouse.sql

host_port() {
  docker inspect --format "{{(index (index .NetworkSettings.Ports \"$1/tcp\") 0).HostPort}}" "${CONTAINER_NAME}"
}

export CLICKHOUSE_TEST_HOST="127.0.0.1"
export CLICKHOUSE_TEST_PORT="$(host_port 9000)"
export CLICKHOUSE_TEST_TLS_PORT="$(host_port 9440)"
export CLICKHOUSE_TEST_USER="${CH_USER}"
export CLICKHOUSE_TEST_PASSWORD="${CH_PASSWORD}"
export CLICKHOUSE_TEST_CA_CERT="${ROOT}/scripts/certs/ca.crt"

echo "==> Running ${BUILD} tests against ${CLICKHOUSE_TEST_HOST}:${CLICKHOUSE_TEST_PORT} (TLS ${CLICKHOUSE_TEST_TLS_PORT}) ..."
if [[ $# -gt 0 ]]; then
  "${UNITTEST}" "$@"
else
  "${UNITTEST}" "test/*"
fi
```

Append to `Makefile`:
```make

#### Tests against a throw-away ClickHouse container (see scripts/test_with_clickhouse.sh)
SMOKE_BUILD ?= release
ARGS ?=

.PHONY: smoke
smoke:
	SMOKE_BUILD=$(SMOKE_BUILD) ./scripts/test_with_clickhouse.sh $(ARGS)
```

- [ ] **Step 4: Fixtures**

`scripts/setup_clickhouse.sql`:
```sql
-- Test fixtures for clickhouse_scanner. Loaded by scripts/test_with_clickhouse.sh (`make smoke`).
SET enable_time_time64_type = 1;

DROP DATABASE IF EXISTS test_db;
CREATE DATABASE test_db;
DROP DATABASE IF EXISTS other_db;
CREATE DATABASE other_db;

CREATE TABLE other_db.other_table (x UInt8) ENGINE = Memory;
INSERT INTO other_db.other_table VALUES (1);

CREATE TABLE test_db.t1 (
    id UInt64,
    name String,
    value Float64,
    created_at DateTime('UTC')
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.t1 VALUES
    (1, 'Alice', 99.5, '2024-01-01 00:00:00'),
    (2, 'Bob', 150.25, '2024-01-02 00:00:00'),
    (3, 'Charlie', 200.0, '2024-01-03 00:00:00');

CREATE VIEW test_db.v1 AS SELECT id, upper(name) AS name FROM test_db.t1;

CREATE TABLE test_db.empty (id UInt64) ENGINE = MergeTree ORDER BY id;

CREATE TABLE test_db.`MixedCase` (`Id` UInt8) ENGINE = Memory;
INSERT INTO test_db.`MixedCase` VALUES (7);

CREATE TABLE test_db.scalars (
    id UInt8,
    b Bool,
    i8 Int8, i16 Int16, i32 Int32, i64 Int64,
    u8 UInt8, u16 UInt16, u32 UInt32, u64 UInt64,
    i128 Int128, u128 UInt128, i256 Int256, u256 UInt256,
    f32 Float32, f64 Float64,
    d9 Decimal(9, 2), d18 Decimal(18, 4), d38 Decimal(38, 10), d76 Decimal(76, 5),
    s String, fs FixedString(3),
    d Date, d32 Date32,
    dt DateTime('UTC'), dt64_3 DateTime64(3, 'UTC'), dt64_9 DateTime64(9, 'Asia/Tokyo'),
    uuid UUID, ip4 IPv4, ip6 IPv6,
    e8 Enum8('red' = 1, 'green' = 2, 'blue' = -3), e16 Enum16('small' = 1000, 'large' = 2000)
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.scalars VALUES
    (1, true,
     -128, -32768, -2147483648, -9223372036854775808,
     255, 65535, 4294967295, 18446744073709551615,
     -170141183460469231731687303715884105728, 340282366920938463463374607431768211455, -1, 1,
     1.5, -2.25,
     1234567.89, 12345678901234.5678, 1234567890123456789012345678.0123456789, 12345.67891,
     'hello', 'abc',
     '2024-02-29', '1900-01-01',
     '2024-02-29 12:34:56', '2024-02-29 12:34:56.789', '2024-02-29 21:34:56.123456789',
     '61f0c404-5cb3-11e7-907b-a6006ad3dba0', '192.168.0.1', '2001:db8::1',
     'blue', 'large'),
    (2, false,
     0, 0, 0, 0,
     0, 0, 0, 0,
     0, 0, 0, 0,
     0, 0,
     0, 0, 0, 0,
     '', 'xyz',
     '1970-01-01', '1970-01-01',
     '1970-01-01 00:00:00', '1970-01-01 00:00:00.000', '1970-01-01 09:00:00.000000000',
     '00000000-0000-0000-0000-000000000000', '0.0.0.0', '::',
     'red', 'small');

CREATE TABLE test_db.nullables (
    id UInt8,
    i Nullable(Int32),
    s Nullable(String),
    d Nullable(Date),
    dt Nullable(DateTime('UTC')),
    e Nullable(Enum8('a' = 1)),
    lc LowCardinality(Nullable(String)),
    lcs LowCardinality(String),
    dec Nullable(Decimal(10, 2)),
    ip Nullable(IPv4)
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.nullables VALUES
    (1, NULL, NULL, NULL, NULL, NULL, NULL, 'x', NULL, NULL),
    (2, 42, 'text', '2024-01-01', '2024-01-01 00:00:00', 'a', 'lc', 'y', 3.14, '10.0.0.1');

CREATE TABLE test_db.nested (
    id UInt8,
    arr Array(Int32),
    arr_null Array(Nullable(String)),
    arr2 Array(Array(UInt8)),
    tup Tuple(a Int32, b String),
    tup_unnamed Tuple(Int32, String),
    m Map(String, UInt64),
    m_ip Map(String, IPv4),
    arr_ip Array(IPv4),
    tup_ip Tuple(ip IPv4, n Int8)
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.nested VALUES
    (1, [1, 2, 3], ['a', NULL], [[1], [2, 3]], (1, 'x'), (2, 'y'),
     {'k1': 1, 'k2': 2}, {'h': '1.2.3.4'}, ['1.1.1.1', '2.2.2.2'], ('8.8.8.8', 5)),
    (2, [], [], [], (0, ''), (0, ''), {}, {}, [], ('0.0.0.0', 0));

CREATE TABLE test_db.semi (
    id UInt8,
    j JSON,
    v Variant(String, UInt64),
    dyn Dynamic
) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.semi VALUES
    (1, '{"a": 1, "b": {"c": "x"}}', 'str', 42),
    (2, '{}', 7, 'hello');

CREATE TABLE test_db.times (id UInt8, t Time, t3 Time64(3), t9 Time64(9)) ENGINE = MergeTree ORDER BY id;
INSERT INTO test_db.times VALUES
    (1, '12:34:56', '12:34:56.789', '12:34:56.123456789'),
    (2, '00:00:00', '00:00:00', '00:00:00');

CREATE TABLE test_db.bad_times (t Time) ENGINE = Memory;
INSERT INTO test_db.bad_times VALUES ('-01:00:00');

CREATE TABLE test_db.aggregates (
    k UInt8,
    total AggregateFunction(sum, UInt64),
    last SimpleAggregateFunction(anyLast, String)
) ENGINE = AggregatingMergeTree ORDER BY k;
INSERT INTO test_db.aggregates SELECT 1, sumState(toUInt64(10)), 'z';

CREATE TABLE test_db.binary (id UInt8, data String) ENGINE = Memory;
INSERT INTO test_db.binary VALUES (1, unhex('FF00'));

CREATE TABLE test_db.big (n UInt64, s String) ENGINE = MergeTree ORDER BY n
AS SELECT number, toString(number) FROM numbers(10000000);
```

- [ ] **Step 5: Run the smoke suite and verify the fixtures**

```bash
make smoke
CLICKHOUSE_TEST_KEEP=1 make smoke ARGS=test/sql/smoke/environment.test
docker exec clickhouse-scanner-test clickhouse-client --user duckdb --password duckdb \
  --query "SELECT count() FROM system.tables WHERE database = 'test_db'"
docker exec clickhouse-scanner-test clickhouse-client --user duckdb --password duckdb --secure \
  --port 9440 --accept-invalid-certificate --query "SELECT 1"
docker rm -f clickhouse-scanner-test
docker ps -a --filter name=clickhouse-scanner-test --format '{{.Names}}'
```
Expected:
- `make smoke`: `All tests passed`, with `environment.test` now actually run instead of skipped, and the log ending with `Removing container`.
- The kept container reports `13` tables, then `1` over TLS.
- The final `docker ps` prints nothing.

Also check that a failing run still cleans up. Run `make smoke ARGS=does/not/exist.test`: it must exit non-zero, and `docker ps -a --filter name=clickhouse-scanner-test` must be empty afterwards. If `enable_time_time64_type` is rejected, check that the image tag is `25.8`.

- [ ] **Step 6: Commit**

```bash
git add scripts Makefile test .gitignore
git commit -m "test: make smoke runs the tests against a throw-away ClickHouse container

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Utils, type parser/mapper and `clickhouse_type_mapping()`

**Files:**
- Create: `src/include/clickhouse_utils.hpp`, `src/clickhouse_utils.cpp`
- Create: `src/include/clickhouse_types.hpp`, `src/clickhouse_types.cpp`
- Create: `src/include/clickhouse_type_mapping_function.hpp`, `src/clickhouse_type_mapping_function.cpp`
- Modify: `src/CMakeLists.txt` (add the three `.cpp` files), `src/clickhouse_scanner_extension.cpp` (register the function)
- Test: `test/sql/types/type_mapping.test`

**Interfaces:**
- Produces (used by every later task):
  - `ClickhouseUtils::QuoteIdentifier(const string &) -> string`: backtick quoting.
  - `ClickhouseUtils::QuoteLiteral(const string &) -> string`: single-quote quoting with backslash escapes.
  - `ClickhouseUtils::IsValidUtf8(const char *, idx_t) -> bool`.
  - `[[noreturn]] ClickhouseUtils::ThrowReadOnly()`.
  - `struct ClickhouseTypeNode { string name; vector<ClickhouseTypeNode> children; vector<string> field_names; vector<string> literals; string text; }`
  - `ClickhouseTypeParser::Parse(const string &) -> ClickhouseTypeNode`. Throws `InvalidInputException("Malformed ClickHouse type ...")`.
  - `struct ClickhouseColumnType { LogicalType type; bool readable = true; }`
  - `ClickhouseTypes::ToDuckDB(const ClickhouseTypeNode &) -> ClickhouseColumnType`.
  - `ClickhouseTypes::ReadExpression(const ClickhouseTypeNode &, const string &expr) -> string`. Returns `expr` unchanged when the type is decoded natively.
  - `ClickhouseTypes::SupportsPushdown(const ClickhouseTypeNode &) -> bool`.
  - `ClickhouseTypes::IsNullable(const ClickhouseTypeNode &) -> bool`.
  - `struct ClickhouseColumnInfo { string name; string clickhouse_type; ClickhouseTypeNode type_node; LogicalType type; bool readable; static ClickhouseColumnInfo Create(const string &name, const string &clickhouse_type); }`
  - SQL: `clickhouse_type_mapping(type VARCHAR) -> (duckdb_type VARCHAR, read_expression VARCHAR, nullable BOOLEAN, pushdown BOOLEAN, readable BOOLEAN)`. The read expression is rendered for a column named `c`.

- [ ] **Step 1: Write the failing test**

`test/sql/types/type_mapping.test`:
```
# name: test/sql/types/type_mapping.test
# description: ClickHouse type strings map to DuckDB types and server-side read expressions
# group: [types]

require clickhouse_scanner

query TTTTT
SELECT * FROM clickhouse_type_mapping('Int32');
----
INTEGER	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Nullable(Int32)');
----
INTEGER	c	true	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('LowCardinality(Nullable(String))');
----
VARCHAR	c	true	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('UInt64');
----
UBIGINT	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Int128');
----
HUGEINT	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('UInt128');
----
UHUGEINT	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Int256');
----
VARCHAR	toString(c)	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Float64');
----
DOUBLE	c	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('BFloat16');
----
FLOAT	toFloat32(c)	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Decimal(10, 2)');
----
DECIMAL(10,2)	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Decimal64(4)');
----
DECIMAL(18,4)	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Decimal(76, 5)');
----
VARCHAR	toString(c)	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('FixedString(3)');
----
VARCHAR	c	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Date32');
----
DATE	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('DateTime(''UTC'')');
----
TIMESTAMP WITH TIME ZONE	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Nullable(DateTime64(9, ''Asia/Tokyo''))');
----
TIMESTAMP WITH TIME ZONE	c	true	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Time');
----
TIME	c	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Time64(9)');
----
TIME_NS	c	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('UUID');
----
UUID	c	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('IPv6');
----
VARCHAR	toString(c)	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Point');
----
VARCHAR	wkt(c)	false	false	true

# enum labels are ordered by their numeric value, like ClickHouse sorts them
query TTTTT
SELECT * FROM clickhouse_type_mapping('Enum8(''red'' = 1, ''green'' = 2, ''blue'' = -3)');
----
ENUM('blue', 'red', 'green')	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Enum16(''it\''s'' = 1000)');
----
ENUM('it''s')	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Array(Int32)');
----
INTEGER[]	c	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Array(IPv4)');
----
VARCHAR[]	arrayMap(x0 -> toString(x0), c)	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Array(Array(Nullable(IPv4)))');
----
VARCHAR[][]	arrayMap(x0 -> arrayMap(x1 -> toString(x1), x0), c)	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Tuple(a Int32, b String)');
----
STRUCT(a INTEGER, b VARCHAR)	c	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Tuple(Int32, String)');
----
STRUCT("1" INTEGER, "2" VARCHAR)	c	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Tuple(ip IPv4, n Int8)');
----
STRUCT(ip VARCHAR, n TINYINT)	tuple(toString(tupleElement(c, 1)), tupleElement(c, 2))	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Array(Tuple(`x y` Int32))');
----
STRUCT("x y" INTEGER)[]	c	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Map(LowCardinality(String), UInt64)');
----
MAP(VARCHAR, UBIGINT)	arrayZip(mapKeys(c), mapValues(c))	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Map(String, IPv4)');
----
MAP(VARCHAR, VARCHAR)	arrayZip(mapKeys(c), arrayMap(x0 -> toString(x0), mapValues(c)))	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('JSON');
----
JSON	toJSONString(c)	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Variant(String, UInt64)');
----
JSON	toJSONString(c)	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('Dynamic');
----
JSON	toJSONString(c)	false	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('SimpleAggregateFunction(anyLast, String)');
----
VARCHAR	c	false	true	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('AggregateFunction(sum, UInt64)');
----
VARCHAR	c	false	false	false

query TTTTT
SELECT * FROM clickhouse_type_mapping('Array(AggregateFunction(sum, UInt64))');
----
VARCHAR[]	c	false	false	false

query TTTTT
SELECT * FROM clickhouse_type_mapping('Nullable(Nothing)');
----
VARCHAR	c	true	false	true

query TTTTT
SELECT * FROM clickhouse_type_mapping('IntervalSecond');
----
VARCHAR	toString(c)	false	false	true

statement error
SELECT * FROM clickhouse_type_mapping('Array(Int32');
----
Malformed ClickHouse type "Array(Int32"

statement error
SELECT * FROM clickhouse_type_mapping('Array(Int32))');
----
Malformed ClickHouse type "Array(Int32))"
```
Note: the expected type strings are DuckDB's `LogicalType::ToString()` renderings. If DuckDB renders a *spelling* differently (for example quoting inside `STRUCT`), change the expectation to DuckDB's spelling. A different *type* is a bug in the mapping.

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && ./build/release/test/unittest test/sql/types/type_mapping.test`
Expected: FAIL with `Catalog Error: Table Function with name clickhouse_type_mapping does not exist!`

- [ ] **Step 3: Implement the utils**

`src/include/clickhouse_utils.hpp`:
```cpp
#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

class ClickhouseUtils {
public:
	//! Quotes an identifier with backticks, escaping backslashes and backticks
	static string QuoteIdentifier(const string &identifier);
	//! Quotes a string literal with single quotes, escaping backslashes and single quotes
	static string QuoteLiteral(const string &literal);
	//! Returns true if the byte range is well-formed UTF-8
	static bool IsValidUtf8(const char *data, idx_t size);
	//! Throws the error used for every write attempt against an attached ClickHouse database
	[[noreturn]] static void ThrowReadOnly();
};

} // namespace duckdb
```

`src/clickhouse_utils.cpp`:
```cpp
#include "clickhouse_utils.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

static string QuoteWith(const string &text, char quote) {
	string result;
	result.reserve(text.size() + 2);
	result += quote;
	for (auto c : text) {
		if (c == '\\' || c == quote) {
			result += '\\';
		}
		result += c;
	}
	result += quote;
	return result;
}

string ClickhouseUtils::QuoteIdentifier(const string &identifier) {
	return QuoteWith(identifier, '`');
}

string ClickhouseUtils::QuoteLiteral(const string &literal) {
	return QuoteWith(literal, '\'');
}

bool ClickhouseUtils::IsValidUtf8(const char *data, idx_t size) {
	auto bytes = reinterpret_cast<const uint8_t *>(data);
	idx_t i = 0;
	while (i < size) {
		auto c = bytes[i];
		if (c < 0x80) {
			i++;
			continue;
		}
		idx_t length;
		uint32_t code_point;
		if ((c & 0xE0) == 0xC0) {
			length = 2;
			code_point = c & 0x1F;
		} else if ((c & 0xF0) == 0xE0) {
			length = 3;
			code_point = c & 0x0F;
		} else if ((c & 0xF8) == 0xF0) {
			length = 4;
			code_point = c & 0x07;
		} else {
			return false;
		}
		if (i + length > size) {
			return false;
		}
		for (idx_t k = 1; k < length; k++) {
			auto continuation = bytes[i + k];
			if ((continuation & 0xC0) != 0x80) {
				return false;
			}
			code_point = (code_point << 6) | (continuation & 0x3F);
		}
		// reject overlong encodings, UTF-16 surrogates and code points beyond U+10FFFF
		if ((length == 2 && code_point < 0x80) || (length == 3 && code_point < 0x800) ||
		    (length == 4 && code_point < 0x10000) || code_point > 0x10FFFF ||
		    (code_point >= 0xD800 && code_point <= 0xDFFF)) {
			return false;
		}
		i += length;
	}
	return true;
}

void ClickhouseUtils::ThrowReadOnly() {
	throw PermissionException("clickhouse_scanner is read-only: writing to ClickHouse is not supported");
}

} // namespace duckdb
```

- [ ] **Step 4: Implement the type parser and mapper**

`src/include/clickhouse_types.hpp`:
```cpp
#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {

//! A parsed ClickHouse type, e.g. Nullable(DateTime64(3, 'UTC'))
struct ClickhouseTypeNode {
	//! Type name, e.g. "Nullable", "DateTime64", "Int32"
	string name;
	//! Arguments that are types (Array(T), Tuple(...), Map(K, V), Nullable(T), ...)
	vector<ClickhouseTypeNode> children;
	//! Element names of named Tuple/Nested arguments (same length as children, empty when unnamed)
	vector<string> field_names;
	//! Literal arguments, verbatim: numbers, quoted strings and enum entries like 'a' = 1
	vector<string> literals;
	//! The (trimmed) text this node was parsed from
	string text;
};

class ClickhouseTypeParser {
public:
	static ClickhouseTypeNode Parse(const string &type_text);
};

struct ClickhouseColumnType {
	LogicalType type;
	//! False when values cannot be read at all (AggregateFunction states)
	bool readable = true;
};

class ClickhouseTypes {
public:
	static ClickhouseColumnType ToDuckDB(const ClickhouseTypeNode &node);
	//! ClickHouse expression that reads `expr` in a form the conversion code decodes; returns `expr` itself for
	//! natively decoded types
	static string ReadExpression(const ClickhouseTypeNode &node, const string &expr);
	//! Whether filters and ORDER BY on a column of this type may be evaluated by ClickHouse
	static bool SupportsPushdown(const ClickhouseTypeNode &node);
	static bool IsNullable(const ClickhouseTypeNode &node);
};

//! A column of a ClickHouse table or query result
struct ClickhouseColumnInfo {
	string name;
	//! Type as reported by ClickHouse, e.g. Nullable(DateTime64(3, 'UTC'))
	string clickhouse_type;
	ClickhouseTypeNode type_node;
	LogicalType type;
	bool readable = true;

	static ClickhouseColumnInfo Create(const string &name, const string &clickhouse_type);
};

} // namespace duckdb
```

`src/clickhouse_types.cpp`:
```cpp
#include "clickhouse_types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unordered_set.hpp"

#include <algorithm>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Parsing
//===--------------------------------------------------------------------===//
static string TrimCopy(const string &text) {
	string result = text;
	StringUtil::Trim(result);
	return result;
}

//! Splits an argument list on top-level commas, respecting parentheses, quotes and backticks
static vector<string> SplitArguments(const string &text, const string &full_type) {
	vector<string> result;
	idx_t depth = 0;
	char quote = '\0';
	string current;
	for (idx_t i = 0; i < text.size(); i++) {
		char c = text[i];
		if (quote != '\0') {
			current += c;
			if (c == '\\' && i + 1 < text.size()) {
				current += text[++i];
			} else if (c == quote) {
				quote = '\0';
			}
			continue;
		}
		if (c == '\'' || c == '`' || c == '"') {
			quote = c;
			current += c;
		} else if (c == '(') {
			depth++;
			current += c;
		} else if (c == ')') {
			if (depth == 0) {
				throw InvalidInputException("Malformed ClickHouse type \"%s\"", full_type);
			}
			depth--;
			current += c;
		} else if (c == ',' && depth == 0) {
			result.push_back(TrimCopy(current));
			current.clear();
		} else {
			current += c;
		}
	}
	if (depth != 0 || quote != '\0') {
		throw InvalidInputException("Malformed ClickHouse type \"%s\"", full_type);
	}
	result.push_back(TrimCopy(current));
	return result;
}

static bool IsLiteralArgument(const string &argument) {
	auto c = argument[0];
	return c == '\'' || c == '-' || c == '+' || StringUtil::CharacterIsDigit(c);
}

//! Splits "name Type" (a named Tuple / Nested element) into name and type. The name stays empty when there is none.
static void SplitFieldName(const string &argument, string &field_name, string &type_text) {
	field_name.clear();
	type_text = argument;
	if (argument[0] == '`' || argument[0] == '"') {
		auto quote = argument[0];
		string name;
		idx_t i = 1;
		for (; i < argument.size(); i++) {
			if (argument[i] == '\\' && i + 1 < argument.size()) {
				name += argument[++i];
			} else if (argument[i] == quote) {
				break;
			} else {
				name += argument[i];
			}
		}
		auto rest = i + 1 < argument.size() ? TrimCopy(argument.substr(i + 1)) : string();
		if (!rest.empty()) {
			field_name = name;
			type_text = rest;
		}
		return;
	}
	for (idx_t i = 0; i < argument.size(); i++) {
		if (argument[i] == '(') {
			return;
		}
		if (StringUtil::CharacterIsSpace(argument[i])) {
			auto rest = TrimCopy(argument.substr(i + 1));
			if (!rest.empty()) {
				field_name = argument.substr(0, i);
				type_text = rest;
			}
			return;
		}
	}
}

ClickhouseTypeNode ClickhouseTypeParser::Parse(const string &type_text) {
	ClickhouseTypeNode node;
	node.text = TrimCopy(type_text);
	if (node.text.empty()) {
		throw InvalidInputException("Malformed ClickHouse type \"%s\"", type_text);
	}
	auto paren = node.text.find('(');
	if (paren == string::npos) {
		node.name = node.text;
		return node;
	}
	if (node.text.back() != ')') {
		throw InvalidInputException("Malformed ClickHouse type \"%s\"", type_text);
	}
	node.name = TrimCopy(node.text.substr(0, paren));
	auto inner = node.text.substr(paren + 1, node.text.size() - paren - 2);
	if (TrimCopy(inner).empty()) {
		return node;
	}
	for (auto &argument : SplitArguments(inner, type_text)) {
		if (argument.empty()) {
			throw InvalidInputException("Malformed ClickHouse type \"%s\"", type_text);
		}
		if (IsLiteralArgument(argument)) {
			node.literals.push_back(argument);
			continue;
		}
		string field_name;
		string child_text;
		SplitFieldName(argument, field_name, child_text);
		node.children.push_back(Parse(child_text));
		node.field_names.push_back(field_name);
	}
	return node;
}

//===--------------------------------------------------------------------===//
// Helpers
//===--------------------------------------------------------------------===//
//! Strips wrappers that do not change how values are decoded
static const ClickhouseTypeNode &Unwrap(const ClickhouseTypeNode &node) {
	if ((node.name == "Nullable" || node.name == "LowCardinality") && node.children.size() == 1) {
		return Unwrap(node.children[0]);
	}
	if (node.name == "SimpleAggregateFunction" && !node.children.empty()) {
		return Unwrap(node.children.back());
	}
	return node;
}

static int64_t ParseIntegerLiteral(const ClickhouseTypeNode &node, idx_t index, int64_t default_value) {
	if (index >= node.literals.size()) {
		return default_value;
	}
	try {
		return std::stoll(node.literals[index]);
	} catch (std::exception &) {
		throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
	}
}

static bool GetDecimalInfo(const ClickhouseTypeNode &node, idx_t &width, idx_t &scale) {
	if (node.name == "Decimal") {
		width = NumericCast<idx_t>(ParseIntegerLiteral(node, 0, 10));
		scale = NumericCast<idx_t>(ParseIntegerLiteral(node, 1, 0));
	} else if (node.name == "Decimal32") {
		width = 9;
		scale = NumericCast<idx_t>(ParseIntegerLiteral(node, 0, 0));
	} else if (node.name == "Decimal64") {
		width = 18;
		scale = NumericCast<idx_t>(ParseIntegerLiteral(node, 0, 0));
	} else if (node.name == "Decimal128") {
		width = 38;
		scale = NumericCast<idx_t>(ParseIntegerLiteral(node, 0, 0));
	} else if (node.name == "Decimal256") {
		width = 76;
		scale = NumericCast<idx_t>(ParseIntegerLiteral(node, 0, 0));
	} else {
		return false;
	}
	return true;
}

struct EnumEntry {
	string label;
	int64_t value;
};

//! Parses entries like 'label' = 5 and returns them ordered by value
static vector<EnumEntry> ParseEnumEntries(const ClickhouseTypeNode &node) {
	vector<EnumEntry> entries;
	for (idx_t i = 0; i < node.literals.size(); i++) {
		auto &literal = node.literals[i];
		if (literal.empty() || literal[0] != '\'') {
			throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
		}
		string label;
		bool closed = false;
		idx_t pos = 1;
		for (; pos < literal.size(); pos++) {
			char c = literal[pos];
			if (c == '\\' && pos + 1 < literal.size()) {
				char escaped = literal[++pos];
				switch (escaped) {
				case 'n':
					label += '\n';
					break;
				case 't':
					label += '\t';
					break;
				case 'r':
					label += '\r';
					break;
				case '0':
					label += '\0';
					break;
				default:
					label += escaped;
				}
			} else if (c == '\'') {
				closed = true;
				pos++;
				break;
			} else {
				label += c;
			}
		}
		if (!closed) {
			throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
		}
		auto rest = TrimCopy(literal.substr(pos));
		int64_t value = NumericCast<int64_t>(i + 1);
		if (!rest.empty()) {
			if (rest[0] != '=') {
				throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
			}
			try {
				value = std::stoll(TrimCopy(rest.substr(1)));
			} catch (std::exception &) {
				throw InvalidInputException("Malformed ClickHouse type \"%s\"", node.text);
			}
		}
		entries.push_back({label, value});
	}
	std::stable_sort(entries.begin(), entries.end(),
	                 [](const EnumEntry &a, const EnumEntry &b) { return a.value < b.value; });
	return entries;
}

static bool IsSemiStructured(const string &name) {
	return name == "JSON" || name == "Object" || name == "Variant" || name == "Dynamic";
}

static bool IsGeoType(const string &name) {
	return name == "Point" || name == "Ring" || name == "LineString" || name == "MultiLineString" ||
	       name == "Polygon" || name == "MultiPolygon" || name == "Geometry";
}

//! Scalar types that the conversion code decodes straight from the native protocol
static bool IsNativeScalar(const string &name) {
	static const unordered_set<string> NATIVE_TYPES = {
	    "Bool",   "Int8",   "Int16",       "Int32", "Int64",      "UInt8",  "UInt16", "UInt32",
	    "UInt64", "Int128", "UInt128",     "Float32", "Float64",  "String", "FixedString", "Date",
	    "Date32", "DateTime", "DateTime64", "Time",  "Time64",     "UUID",   "Enum8",  "Enum16",
	    "Nothing", "AggregateFunction"};
	return NATIVE_TYPES.find(name) != NATIVE_TYPES.end();
}

//===--------------------------------------------------------------------===//
// ToDuckDB
//===--------------------------------------------------------------------===//
ClickhouseColumnType ClickhouseTypes::ToDuckDB(const ClickhouseTypeNode &node) {
	auto &type = Unwrap(node);
	auto &name = type.name;
	ClickhouseColumnType result;
	result.type = LogicalType::VARCHAR;
	idx_t width;
	idx_t scale;
	if (name == "Bool") {
		result.type = LogicalType::BOOLEAN;
	} else if (name == "Int8") {
		result.type = LogicalType::TINYINT;
	} else if (name == "Int16") {
		result.type = LogicalType::SMALLINT;
	} else if (name == "Int32") {
		result.type = LogicalType::INTEGER;
	} else if (name == "Int64") {
		result.type = LogicalType::BIGINT;
	} else if (name == "UInt8") {
		result.type = LogicalType::UTINYINT;
	} else if (name == "UInt16") {
		result.type = LogicalType::USMALLINT;
	} else if (name == "UInt32") {
		result.type = LogicalType::UINTEGER;
	} else if (name == "UInt64") {
		result.type = LogicalType::UBIGINT;
	} else if (name == "Int128") {
		result.type = LogicalType::HUGEINT;
	} else if (name == "UInt128") {
		result.type = LogicalType::UHUGEINT;
	} else if (name == "Float32" || name == "BFloat16") {
		result.type = LogicalType::FLOAT;
	} else if (name == "Float64") {
		result.type = LogicalType::DOUBLE;
	} else if (GetDecimalInfo(type, width, scale)) {
		if (width <= 38) {
			result.type = LogicalType::DECIMAL(NumericCast<uint8_t>(width), NumericCast<uint8_t>(scale));
		}
	} else if (name == "String" || name == "FixedString") {
		result.type = LogicalType::VARCHAR;
	} else if (name == "Date" || name == "Date32") {
		result.type = LogicalType::DATE;
	} else if (name == "DateTime" || name == "DateTime64") {
		result.type = LogicalType::TIMESTAMP_TZ;
	} else if (name == "Time") {
		result.type = LogicalType::TIME;
	} else if (name == "Time64") {
		result.type = ParseIntegerLiteral(type, 0, 3) > 6 ? LogicalType::TIME_NS : LogicalType::TIME;
	} else if (name == "UUID") {
		result.type = LogicalType::UUID;
	} else if ((name == "Enum8" || name == "Enum16") && !type.literals.empty()) {
		auto entries = ParseEnumEntries(type);
		Vector labels(LogicalType::VARCHAR, entries.size());
		auto label_data = FlatVector::GetData<string_t>(labels);
		for (idx_t i = 0; i < entries.size(); i++) {
			label_data[i] = StringVector::AddString(labels, entries[i].label);
		}
		result.type = LogicalType::ENUM(labels, entries.size());
	} else if (name == "Array" && type.children.size() == 1) {
		auto child = ToDuckDB(type.children[0]);
		result.type = LogicalType::LIST(child.type);
		result.readable = child.readable;
	} else if (name == "Tuple" && !type.children.empty()) {
		child_list_t<LogicalType> children;
		for (idx_t i = 0; i < type.children.size(); i++) {
			auto child = ToDuckDB(type.children[i]);
			result.readable = result.readable && child.readable;
			auto field_name = type.field_names[i].empty() ? to_string(i + 1) : type.field_names[i];
			children.emplace_back(field_name, child.type);
		}
		result.type = LogicalType::STRUCT(std::move(children));
	} else if (name == "Map" && type.children.size() == 2) {
		auto key = ToDuckDB(type.children[0]);
		auto value = ToDuckDB(type.children[1]);
		result.type = LogicalType::MAP(key.type, value.type);
		result.readable = key.readable && value.readable;
	} else if (IsSemiStructured(name)) {
		result.type = LogicalType::JSON();
	} else if (name == "AggregateFunction") {
		result.readable = false;
	}
	// everything else (Nothing, IPv4/IPv6, (U)Int256, Decimal256, geo types, intervals, ...) is VARCHAR
	return result;
}

//===--------------------------------------------------------------------===//
// Read expressions
//===--------------------------------------------------------------------===//
static string ReadExpressionInternal(const ClickhouseTypeNode &node, const string &expr, idx_t depth);

//! Applies the element read expression to every element of an array expression
static string ArrayReadExpression(const ClickhouseTypeNode &element, const string &array_expr, idx_t depth) {
	auto variable = "x" + to_string(depth);
	auto element_expr = ReadExpressionInternal(element, variable, depth + 1);
	if (element_expr == variable) {
		return array_expr;
	}
	return "arrayMap(" + variable + " -> " + element_expr + ", " + array_expr + ")";
}

static string ReadExpressionInternal(const ClickhouseTypeNode &node, const string &expr, idx_t depth) {
	auto &type = Unwrap(node);
	auto &name = type.name;
	idx_t width;
	idx_t scale;
	if (GetDecimalInfo(type, width, scale)) {
		return width <= 38 ? expr : "toString(" + expr + ")";
	}
	if (IsNativeScalar(name)) {
		return expr;
	}
	if (name == "BFloat16") {
		return "toFloat32(" + expr + ")";
	}
	if (IsGeoType(name)) {
		return "wkt(" + expr + ")";
	}
	if (IsSemiStructured(name)) {
		return "toJSONString(" + expr + ")";
	}
	if (name == "Array" && type.children.size() == 1) {
		return ArrayReadExpression(type.children[0], expr, depth);
	}
	if (name == "Tuple" && !type.children.empty()) {
		vector<string> elements;
		bool changed = false;
		for (idx_t i = 0; i < type.children.size(); i++) {
			auto element = "tupleElement(" + expr + ", " + to_string(i + 1) + ")";
			auto element_expr = ReadExpressionInternal(type.children[i], element, depth + 1);
			changed = changed || element_expr != element;
			elements.push_back(element_expr);
		}
		if (!changed) {
			return expr;
		}
		return "tuple(" + StringUtil::Join(elements, ", ") + ")";
	}
	if (name == "Map" && type.children.size() == 2) {
		// Map(K, V) is read as Array(Tuple(K, V)): the same layout as a DuckDB MAP
		return "arrayZip(" + ArrayReadExpression(type.children[0], "mapKeys(" + expr + ")", depth) + ", " +
		       ArrayReadExpression(type.children[1], "mapValues(" + expr + ")", depth) + ")";
	}
	return "toString(" + expr + ")";
}

string ClickhouseTypes::ReadExpression(const ClickhouseTypeNode &node, const string &expr) {
	return ReadExpressionInternal(node, expr, 0);
}

bool ClickhouseTypes::SupportsPushdown(const ClickhouseTypeNode &node) {
	static const unordered_set<string> PUSHDOWN_TYPES = {
	    "Bool",   "Int8",   "Int16", "Int32",  "Int64",    "UInt8",      "UInt16", "UInt32",
	    "UInt64", "Int128", "UInt128", "String", "Date",   "Date32",     "DateTime", "DateTime64",
	    "Enum8",  "Enum16"};
	auto &type = Unwrap(node);
	idx_t width;
	idx_t scale;
	if (GetDecimalInfo(type, width, scale)) {
		return width <= 38;
	}
	return PUSHDOWN_TYPES.find(type.name) != PUSHDOWN_TYPES.end();
}

bool ClickhouseTypes::IsNullable(const ClickhouseTypeNode &node) {
	if (node.name == "Nullable") {
		return true;
	}
	if ((node.name == "LowCardinality" || node.name == "SimpleAggregateFunction") && !node.children.empty()) {
		return IsNullable(node.children.back());
	}
	return false;
}

ClickhouseColumnInfo ClickhouseColumnInfo::Create(const string &name, const string &clickhouse_type) {
	ClickhouseColumnInfo result;
	result.name = name;
	result.clickhouse_type = clickhouse_type;
	result.type_node = ClickhouseTypeParser::Parse(clickhouse_type);
	auto mapped = ClickhouseTypes::ToDuckDB(result.type_node);
	result.type = std::move(mapped.type);
	result.readable = mapped.readable;
	return result;
}

} // namespace duckdb
```

- [ ] **Step 5: Implement `clickhouse_type_mapping()`**

`src/include/clickhouse_type_mapping_function.hpp`:
```cpp
#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

//! clickhouse_type_mapping('<ClickHouse type>'): shows how a ClickHouse type is mapped and read
class ClickhouseTypeMappingFunction : public TableFunction {
public:
	ClickhouseTypeMappingFunction();
};

} // namespace duckdb
```

`src/clickhouse_type_mapping_function.cpp`:
```cpp
#include "clickhouse_type_mapping_function.hpp"

#include "clickhouse_types.hpp"

namespace duckdb {

struct ClickhouseTypeMappingData : public TableFunctionData {
	string duckdb_type;
	string read_expression;
	bool nullable = false;
	bool pushdown = false;
	bool readable = true;
	bool finished = false;
};

static unique_ptr<FunctionData> TypeMappingBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("clickhouse_type_mapping: the type cannot be NULL");
	}
	auto node = ClickhouseTypeParser::Parse(StringValue::Get(input.inputs[0]));
	auto mapped = ClickhouseTypes::ToDuckDB(node);
	auto result = make_uniq<ClickhouseTypeMappingData>();
	result->duckdb_type = mapped.type.ToString();
	result->read_expression = ClickhouseTypes::ReadExpression(node, "c");
	result->nullable = ClickhouseTypes::IsNullable(node);
	result->pushdown = ClickhouseTypes::SupportsPushdown(node);
	result->readable = mapped.readable;

	names = {"duckdb_type", "read_expression", "nullable", "pushdown", "readable"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BOOLEAN,
	                LogicalType::BOOLEAN};
	return std::move(result);
}

static void TypeMappingFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<ClickhouseTypeMappingData>();
	if (data.finished) {
		return;
	}
	output.SetValue(0, 0, Value(data.duckdb_type));
	output.SetValue(1, 0, Value(data.read_expression));
	output.SetValue(2, 0, Value::BOOLEAN(data.nullable));
	output.SetValue(3, 0, Value::BOOLEAN(data.pushdown));
	output.SetValue(4, 0, Value::BOOLEAN(data.readable));
	output.SetCardinality(1);
	data.finished = true;
}

ClickhouseTypeMappingFunction::ClickhouseTypeMappingFunction()
    : TableFunction("clickhouse_type_mapping", {LogicalType::VARCHAR}, TypeMappingFunction, TypeMappingBind) {
}

} // namespace duckdb
```

Register it in `src/clickhouse_scanner_extension.cpp`. Add the include `#include "clickhouse_type_mapping_function.hpp"`, and inside `LoadInternal` after the version function add:
```cpp
	loader.RegisterFunction(ClickhouseTypeMappingFunction());
```

Update `src/CMakeLists.txt`:
```cmake
add_subdirectory(storage)

add_library(
  clickhouse_ext OBJECT
  clickhouse_scanner_extension.cpp
  clickhouse_type_mapping_function.cpp
  clickhouse_types.cpp
  clickhouse_utils.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:clickhouse_ext>
    PARENT_SCOPE)
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `make release && ./build/release/test/unittest test/sql/types/type_mapping.test`
Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src test
git commit -m "feat: ClickHouse type parser, DuckDB type mapping and clickhouse_type_mapping()

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Connection config, secrets, connections, pool and `ATTACH`

**Files:**
- Create: `src/include/clickhouse_connection_config.hpp`, `src/clickhouse_connection_config.cpp`
- Create: `src/include/clickhouse_secrets.hpp`, `src/clickhouse_secrets.cpp`
- Create: `src/include/clickhouse_connection.hpp`, `src/clickhouse_connection.cpp`
- Create: `src/include/storage/clickhouse_connection_pool.hpp`, `src/storage/clickhouse_connection_pool.cpp`
- Create: `src/include/storage/clickhouse_transaction.hpp`, `src/storage/clickhouse_transaction.cpp`
- Create: `src/include/storage/clickhouse_catalog_set.hpp`, `src/storage/clickhouse_catalog_set.cpp`
- Create: `src/include/storage/clickhouse_schema_set.hpp`, `src/storage/clickhouse_schema_set.cpp`
- Create: `src/include/storage/clickhouse_schema_entry.hpp`, `src/storage/clickhouse_schema_entry.cpp`
- Create: `src/include/storage/clickhouse_catalog.hpp`, `src/storage/clickhouse_catalog.cpp`
- Create: `src/include/storage/clickhouse_storage_extension.hpp`, `src/storage/clickhouse_storage_extension.cpp`
- Modify: `src/CMakeLists.txt`, `src/storage/CMakeLists.txt`, `src/clickhouse_scanner_extension.cpp`
- Test: `test/sql/attach/attach_errors.test`, `test/sql/attach/secrets.test`, `test/sql/attach/attach.test`, `test/sql/attach/tls.test`

**Interfaces:**
- Consumes: `ClickhouseUtils::*` (Task 3).
- Produces:
  - `struct ClickhouseConnectionConfig` with fields `host, port, user, password, database, secure (int8: -1 unset), ca_cert, skip_verify, compression, settings (vector<pair<string,string>>)`. Methods: `ApplyConnectionString(const string &)`, `SetOption(const string &key, const string &value)`, `AddSettings(const string &)`, `IsSecure()`, `GetPort()`, `ToDisplayString()`, `static OptionNames()`.
  - `ClickhouseSecrets::{CreateType, CreateFunction, SetSecretParameters, GetSecretEntry(ClientContext&, const string&), ApplySecret(const SecretEntry&, ClickhouseConnectionConfig&)}`.
  - `struct ClickhouseTimeouts { connect_timeout_ms; receive_timeout_ms; static FromContext(ClientContext&); }`.
  - `class ClickhouseConnection` with:
    - `static Open(config, timeouts) -> unique_ptr<ClickhouseConnection>`
    - `BeginQuery(const string &sql)`
    - `NextBlock() -> std::optional<clickhouse::Block>`
    - `Cancel()`, `IsQueryRunning()`
    - `Query(const string &sql) -> vector<clickhouse::Block>`
    - `IsHealthy()`, `IsBroken()`
    - `static SetDebugPrintQueries(bool)`
  - `using ClickhousePoolConnection = dbconnector::pool::PooledConnection<ClickhouseConnection>`. `class ClickhouseConnectionPool` with `GetConnection() -> ClickhousePoolConnection` and `static PoolConfigFromContext(ClientContext&)`.
  - `class ClickhouseCatalog` with:
    - `CATALOG_TYPE = "clickhouse"`
    - `GetConfig()`, `GetAttachOptions()`
    - `GetConnectionPool() -> ClickhouseConnectionPool&`, `GetConnectionPoolPtr() -> shared_ptr<ClickhouseConnectionPool>`
    - `ClearCache()`
  - `class ClickhouseCatalogSet` (lazy cache): `GetEntry(ClientContext&, const string&)`, `Scan(ClientContext&, callback)`, `ClearEntries()`, `protected: virtual LoadEntries(ClientContext&)`, `CreateEntry(unique_ptr<CatalogEntry>)`.
  - `class ClickhouseSchemaEntry` (tables arrive in Task 5).

- [ ] **Step 1: Write the failing tests**

`test/sql/attach/attach_errors.test`:
```
# name: test/sql/attach/attach_errors.test
# description: invalid ATTACH input is rejected before connecting
# group: [attach]

require clickhouse_scanner

statement error
ATTACH 'hots=localhost' AS ch (TYPE clickhouse);
----
Unknown ClickHouse connection option "hots"

statement error
ATTACH 'host=localhost port=abc' AS ch (TYPE clickhouse);
----
Invalid ClickHouse port "abc"

statement error
ATTACH 'host=localhost compression=snappy' AS ch (TYPE clickhouse);
----
Invalid value "snappy" for ClickHouse option "compression"

statement error
ATTACH 'host=localhost secure=maybe' AS ch (TYPE clickhouse);
----
Invalid value "maybe" for ClickHouse option "secure"

statement error
ATTACH 'host=localhost password=''unterminated' AS ch (TYPE clickhouse);
----
unterminated quoted value

statement error
ATTACH 'host=localhost settings=''bad setting''' AS ch (TYPE clickhouse);
----
Invalid ClickHouse setting "bad setting"

statement error
ATTACH 'clickhouse://localhost:9000/db?bogus=1' AS ch (TYPE clickhouse);
----
Unknown ClickHouse connection option "bogus"

statement error
ATTACH '' AS ch (TYPE clickhouse, SECRET does_not_exist);
----
Secret with name "does_not_exist" not found

statement error
ATTACH '' AS ch (TYPE clickhouse, FOO 1);
----
Unrecognized option for ClickHouse attach

statement error
ATTACH 'host=127.0.0.1 port=1 connect_timeout=1' AS ch (TYPE clickhouse);
----
Unknown ClickHouse connection option "connect_timeout"

statement ok
SET ch_connect_timeout_ms = 2000;

statement error
ATTACH 'host=127.0.0.1 port=1' AS ch (TYPE clickhouse);
----
Failed to connect to ClickHouse at 127.0.0.1:1
```

`test/sql/attach/secrets.test`:
```
# name: test/sql/attach/secrets.test
# description: CREATE SECRET (TYPE clickhouse) validates options and redacts the password
# group: [attach]

require clickhouse_scanner

statement ok
CREATE SECRET ch_secret (TYPE clickhouse, HOST 'example.com', PORT 9440, USER 'alice', PASSWORD 'hunter2', DATABASE 'analytics', SECURE true);

query I
SELECT secret_string LIKE '%password=redacted%' AND secret_string NOT LIKE '%hunter2%' FROM duckdb_secrets() WHERE name = 'ch_secret';
----
true

statement error
CREATE SECRET bad_secret (TYPE clickhouse, HOST 'example.com', COMPRESSION 'snappy');
----
Invalid value "snappy" for ClickHouse option "compression"
```

`test/sql/attach/attach.test`:
```
# name: test/sql/attach/attach.test
# description: ATTACH through key=value strings, URIs and secrets
# group: [attach]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

# the path never contains the password
query II
SELECT type, path = 'clickhouse://${CLICKHOUSE_TEST_USER}@${CLICKHOUSE_TEST_HOST}:${CLICKHOUSE_TEST_PORT}/test_db' FROM duckdb_databases() WHERE database_name = 'ch';
----
clickhouse	true

query I
SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'ch' ORDER BY schema_name;
----
default
other_db
test_db

statement error
CREATE SCHEMA ch.new_db;
----
clickhouse_scanner is read-only

statement ok
DETACH ch;

# the storage extension is also reachable under its full name
statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch_full (TYPE clickhouse_scanner);

# the unnamed secret supplies credentials; the URI supplies host, port and database
statement ok
CREATE SECRET (TYPE clickhouse, USER '${CLICKHOUSE_TEST_USER}', PASSWORD '${CLICKHOUSE_TEST_PASSWORD}');

statement ok
ATTACH 'clickhouse://${CLICKHOUSE_TEST_HOST}:${CLICKHOUSE_TEST_PORT}/other_db' AS ch_uri (TYPE clickhouse);

query I
SELECT path = 'clickhouse://${CLICKHOUSE_TEST_USER}@${CLICKHOUSE_TEST_HOST}:${CLICKHOUSE_TEST_PORT}/other_db' FROM duckdb_databases() WHERE database_name = 'ch_uri';
----
true

statement ok
CREATE SECRET named_secret (TYPE clickhouse, HOST '${CLICKHOUSE_TEST_HOST}', PORT '${CLICKHOUSE_TEST_PORT}', USER '${CLICKHOUSE_TEST_USER}', PASSWORD '${CLICKHOUSE_TEST_PASSWORD}', DATABASE 'test_db');

statement ok
ATTACH '' AS ch_secret (TYPE clickhouse, SECRET named_secret);

query I
SELECT path = 'clickhouse://${CLICKHOUSE_TEST_USER}@${CLICKHOUSE_TEST_HOST}:${CLICKHOUSE_TEST_PORT}/test_db' FROM duckdb_databases() WHERE database_name = 'ch_secret';
----
true

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD}' AS ch_sys (TYPE clickhouse, SHOW_SYSTEM true);

query I
SELECT count(*) FROM duckdb_schemas() WHERE database_name = 'ch_sys' AND schema_name = 'system';
----
1

statement error
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=wrong' AS ch_bad (TYPE clickhouse);
----
Failed to connect to ClickHouse at
```

`test/sql/attach/tls.test`:
```
# name: test/sql/attach/tls.test
# description: TLS connections on the secure native port
# group: [attach]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_TLS_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

require-env CLICKHOUSE_TEST_CA_CERT

# the mapped TLS port is random, so secure=true must be explicit
statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_TLS_PORT} secure=true user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db ca_cert=${CLICKHOUSE_TEST_CA_CERT}' AS ch_tls (TYPE clickhouse);

query I
SELECT path = 'clickhouses://${CLICKHOUSE_TEST_USER}@${CLICKHOUSE_TEST_HOST}:${CLICKHOUSE_TEST_TLS_PORT}/test_db' FROM duckdb_databases() WHERE database_name = 'ch_tls';
----
true

query I
SELECT count(*) FROM ch_tls.test_db.t1;
----
3

# the self-signed test certificate is rejected without the test CA
statement error
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_TLS_PORT} secure=true user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD}' AS ch_tls_bad (TYPE clickhouse);
----
Failed to connect to ClickHouse at

statement ok
ATTACH 'clickhouses://${CLICKHOUSE_TEST_USER}:${CLICKHOUSE_TEST_PASSWORD}@${CLICKHOUSE_TEST_HOST}:${CLICKHOUSE_TEST_TLS_PORT}/test_db?skip_verify=true' AS ch_tls_skip (TYPE clickhouse);
```
Note: `SELECT count(*) FROM ch_tls.test_db.t1` only works from Task 6 onward. In Task 4, replace it with `SELECT count(*) FROM duckdb_schemas() WHERE database_name = 'ch_tls' AND schema_name = 'test_db'` (expected `1`), and switch to the scan in Task 6.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make release && make smoke ARGS='test/sql/attach/*'`
Expected: FAIL. The first error is about the unknown storage type `clickhouse` / the missing secret type.

- [ ] **Step 3: Connection config**

`src/include/clickhouse_connection_config.hpp`:
```cpp
#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/pair.hpp"

namespace duckdb {

//! Everything needed to open a ClickHouse connection. Built from (in order) a secret, the ATTACH path and the
//! ATTACH SETTINGS option; later sources override earlier ones.
struct ClickhouseConnectionConfig {
	string host = "localhost";
	//! 0 = not set: 9440 when secure, 9000 otherwise
	uint16_t port = 0;
	string user = "default";
	string password;
	string database = "default";
	//! -1 = not set (secure when port is 9440), 0 = false, 1 = true
	int8_t secure = -1;
	string ca_cert;
	bool skip_verify = false;
	//! lz4, zstd or none
	string compression = "lz4";
	//! ClickHouse settings sent with every query
	vector<pair<string, string>> settings;

	//! Applies a "key=value key2='quoted value'" string or a clickhouse[s]://user:password@host:port/db?k=v URI
	void ApplyConnectionString(const string &connection_string);
	//! Applies one option (case-insensitive key, aliases allowed). Throws InvalidInputException on unknown keys
	void SetOption(const string &key, const string &value);
	//! Appends settings written as "name1=value1,name2=value2"
	void AddSettings(const string &settings_text);

	bool IsSecure() const;
	uint16_t GetPort() const;
	//! clickhouse[s]://user@host:port/database (never includes the password)
	string ToDisplayString() const;

	//! All accepted option names, including aliases
	static const vector<string> &OptionNames();

private:
	void ApplyUri(const string &uri);
	void ApplyKeyValuePairs(const string &text);
};

} // namespace duckdb
```

`src/clickhouse_connection_config.cpp`:
```cpp
#include "clickhouse_connection_config.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

const vector<string> &ClickhouseConnectionConfig::OptionNames() {
	static const vector<string> OPTION_NAMES = {"host",     "port",        "user",        "password", "database",
	                                            "secure",   "ca_cert",     "skip_verify", "compression",
	                                            "settings", "username",    "dbname",      "hostname"};
	return OPTION_NAMES;
}

static bool ParseBoolean(const string &key, const string &value) {
	auto lower = StringUtil::Lower(value);
	if (lower == "true" || lower == "1" || lower == "yes" || lower == "on") {
		return true;
	}
	if (lower == "false" || lower == "0" || lower == "no" || lower == "off") {
		return false;
	}
	throw InvalidInputException("Invalid value \"%s\" for ClickHouse option \"%s\": expected a boolean", value, key);
}

static uint16_t ParsePort(const string &value) {
	if (value.empty() || value.size() > 5) {
		throw InvalidInputException("Invalid ClickHouse port \"%s\"", value);
	}
	uint32_t port = 0;
	for (auto c : value) {
		if (!StringUtil::CharacterIsDigit(c)) {
			throw InvalidInputException("Invalid ClickHouse port \"%s\"", value);
		}
		port = port * 10 + static_cast<uint32_t>(c - '0');
	}
	if (port == 0 || port > 65535) {
		throw InvalidInputException("Invalid ClickHouse port \"%s\"", value);
	}
	return static_cast<uint16_t>(port);
}

static bool IsValidSettingName(const string &name) {
	if (name.empty() || !(StringUtil::CharacterIsAlpha(name[0]) || name[0] == '_')) {
		return false;
	}
	for (auto c : name) {
		if (!(StringUtil::CharacterIsAlphaNumeric(c) || c == '_')) {
			return false;
		}
	}
	return true;
}

void ClickhouseConnectionConfig::SetOption(const string &key_p, const string &value) {
	auto key = StringUtil::Lower(key_p);
	if (key == "username") {
		key = "user";
	} else if (key == "dbname") {
		key = "database";
	} else if (key == "hostname") {
		key = "host";
	}
	if (key == "host") {
		if (value.empty()) {
			throw InvalidInputException("ClickHouse option \"host\" cannot be empty");
		}
		host = value;
	} else if (key == "port") {
		port = ParsePort(value);
	} else if (key == "user") {
		user = value;
	} else if (key == "password") {
		password = value;
	} else if (key == "database") {
		database = value;
	} else if (key == "secure") {
		secure = ParseBoolean(key, value) ? 1 : 0;
	} else if (key == "ca_cert") {
		ca_cert = value;
	} else if (key == "skip_verify") {
		skip_verify = ParseBoolean(key, value);
	} else if (key == "compression") {
		auto lower = StringUtil::Lower(value);
		if (lower != "lz4" && lower != "zstd" && lower != "none") {
			throw InvalidInputException(
			    "Invalid value \"%s\" for ClickHouse option \"compression\": expected lz4, zstd or none", value);
		}
		compression = lower;
	} else if (key == "settings") {
		AddSettings(value);
	} else {
		throw InvalidInputException("Unknown ClickHouse connection option \"%s\"", key_p);
	}
}

void ClickhouseConnectionConfig::AddSettings(const string &settings_text) {
	for (auto &entry : StringUtil::Split(settings_text, ',')) {
		auto item = entry;
		StringUtil::Trim(item);
		if (item.empty()) {
			continue;
		}
		auto equals = item.find('=');
		string name = equals == string::npos ? item : item.substr(0, equals);
		StringUtil::Trim(name);
		if (equals == string::npos || !IsValidSettingName(name)) {
			throw InvalidInputException("Invalid ClickHouse setting \"%s\": expected name=value", item);
		}
		auto value = item.substr(equals + 1);
		StringUtil::Trim(value);
		settings.emplace_back(name, value);
	}
}

void ClickhouseConnectionConfig::ApplyConnectionString(const string &connection_string) {
	auto text = connection_string;
	StringUtil::Trim(text);
	if (text.empty()) {
		return;
	}
	if (StringUtil::StartsWith(text, "clickhouse://") || StringUtil::StartsWith(text, "clickhouses://")) {
		ApplyUri(text);
	} else {
		ApplyKeyValuePairs(text);
	}
}

void ClickhouseConnectionConfig::ApplyKeyValuePairs(const string &text) {
	idx_t pos = 0;
	while (true) {
		while (pos < text.size() && StringUtil::CharacterIsSpace(text[pos])) {
			pos++;
		}
		if (pos >= text.size()) {
			return;
		}
		string key;
		while (pos < text.size() && text[pos] != '=' && !StringUtil::CharacterIsSpace(text[pos])) {
			key += text[pos++];
		}
		while (pos < text.size() && StringUtil::CharacterIsSpace(text[pos])) {
			pos++;
		}
		if (pos >= text.size() || text[pos] != '=') {
			throw InvalidInputException("Invalid ClickHouse connection string: expected \"=\" after \"%s\"", key);
		}
		pos++;
		while (pos < text.size() && StringUtil::CharacterIsSpace(text[pos])) {
			pos++;
		}
		string value;
		if (pos < text.size() && text[pos] == '\'') {
			pos++;
			bool closed = false;
			while (pos < text.size()) {
				if (text[pos] == '\\' && pos + 1 < text.size()) {
					value += text[pos + 1];
					pos += 2;
				} else if (text[pos] == '\'') {
					closed = true;
					pos++;
					break;
				} else {
					value += text[pos++];
				}
			}
			if (!closed) {
				throw InvalidInputException(
				    "Invalid ClickHouse connection string: unterminated quoted value for \"%s\"", key);
			}
		} else {
			while (pos < text.size() && !StringUtil::CharacterIsSpace(text[pos])) {
				value += text[pos++];
			}
		}
		SetOption(key, value);
	}
}

void ClickhouseConnectionConfig::ApplyUri(const string &uri) {
	bool secure_scheme = StringUtil::StartsWith(uri, "clickhouses://");
	auto rest = uri.substr(secure_scheme ? 14 : 13);
	if (secure_scheme) {
		secure = 1;
	}
	string query;
	auto question = rest.find('?');
	if (question != string::npos) {
		query = rest.substr(question + 1);
		rest = rest.substr(0, question);
	}
	string path;
	auto slash = rest.find('/');
	if (slash != string::npos) {
		path = rest.substr(slash + 1);
		rest = rest.substr(0, slash);
	}
	auto at = rest.rfind('@');
	if (at != string::npos) {
		auto user_info = rest.substr(0, at);
		rest = rest.substr(at + 1);
		auto colon = user_info.find(':');
		if (colon == string::npos) {
			user = StringUtil::URLDecode(user_info);
		} else {
			user = StringUtil::URLDecode(user_info.substr(0, colon));
			password = StringUtil::URLDecode(user_info.substr(colon + 1));
		}
	}
	if (!rest.empty() && rest[0] == '[') {
		// IPv6 literal: [::1]:9000
		auto close = rest.find(']');
		if (close == string::npos) {
			throw InvalidInputException("Invalid ClickHouse URI \"%s\": unterminated IPv6 address", uri);
		}
		host = rest.substr(1, close - 1);
		rest = rest.substr(close + 1);
		if (!rest.empty()) {
			if (rest[0] != ':') {
				throw InvalidInputException("Invalid ClickHouse URI \"%s\"", uri);
			}
			port = ParsePort(rest.substr(1));
		}
	} else {
		auto colon = rest.rfind(':');
		if (colon != string::npos) {
			port = ParsePort(rest.substr(colon + 1));
			rest = rest.substr(0, colon);
		}
		if (!rest.empty()) {
			host = rest;
		}
	}
	if (!path.empty()) {
		database = StringUtil::URLDecode(path);
	}
	for (auto &parameter : StringUtil::Split(query, '&')) {
		if (parameter.empty()) {
			continue;
		}
		auto equals = parameter.find('=');
		if (equals == string::npos) {
			throw InvalidInputException("Invalid ClickHouse URI parameter \"%s\": expected key=value", parameter);
		}
		SetOption(StringUtil::URLDecode(parameter.substr(0, equals)),
		          StringUtil::URLDecode(parameter.substr(equals + 1)));
	}
}

bool ClickhouseConnectionConfig::IsSecure() const {
	if (secure == -1) {
		return port == 9440;
	}
	return secure == 1;
}

uint16_t ClickhouseConnectionConfig::GetPort() const {
	if (port != 0) {
		return port;
	}
	return IsSecure() ? 9440 : 9000;
}

string ClickhouseConnectionConfig::ToDisplayString() const {
	return StringUtil::Format("%s://%s@%s:%d/%s", IsSecure() ? "clickhouses" : "clickhouse", user, host,
	                          static_cast<int32_t>(GetPort()), database);
}

} // namespace duckdb
```

- [ ] **Step 4: Secrets**

`src/include/clickhouse_secrets.hpp`:
```cpp
#pragma once

#include "clickhouse_connection_config.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {

class ClickhouseSecrets {
public:
	static constexpr const char *TYPE_NAME = "clickhouse";
	//! Name DuckDB gives an unnamed CREATE SECRET (TYPE clickhouse, ...)
	static constexpr const char *DEFAULT_SECRET_NAME = "__default_clickhouse";

	static SecretType CreateType();
	static unique_ptr<BaseSecret> CreateFunction(ClientContext &context, CreateSecretInput &input);
	static void SetSecretParameters(CreateSecretFunction &function);
	//! The named secret, or the default unnamed ClickHouse secret when secret_name is empty (may return nullptr).
	//! Throws when a named secret does not exist.
	static unique_ptr<SecretEntry> GetSecretEntry(ClientContext &context, const string &secret_name);
	static void ApplySecret(const SecretEntry &entry, ClickhouseConnectionConfig &config);
};

} // namespace duckdb
```

`src/clickhouse_secrets.cpp`:
```cpp
#include "clickhouse_secrets.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

SecretType ClickhouseSecrets::CreateType() {
	SecretType secret_type;
	secret_type.name = TYPE_NAME;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	return secret_type;
}

unique_ptr<BaseSecret> ClickhouseSecrets::CreateFunction(ClientContext &context, CreateSecretInput &input) {
	vector<string> prefix_paths;
	auto result = make_uniq<KeyValueSecret>(prefix_paths, TYPE_NAME, "config", input.name);
	// validate every option now so that a broken secret fails at CREATE SECRET rather than at ATTACH
	ClickhouseConnectionConfig validation;
	for (auto &named_param : input.options) {
		auto key = StringUtil::Lower(named_param.first);
		auto value = named_param.second.ToString();
		validation.SetOption(key, value);
		result->secret_map[key] = Value(value);
	}
	result->redact_keys = {"password"};
	return std::move(result);
}

void ClickhouseSecrets::SetSecretParameters(CreateSecretFunction &function) {
	for (auto &name : ClickhouseConnectionConfig::OptionNames()) {
		function.named_parameters[name] = LogicalType::VARCHAR;
	}
}

unique_ptr<SecretEntry> ClickhouseSecrets::GetSecretEntry(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto name = secret_name.empty() ? string(DEFAULT_SECRET_NAME) : secret_name;
	auto entry = secret_manager.GetSecretByName(transaction, name);
	if (!entry) {
		entry = secret_manager.GetSecretByName(transaction, name, "local_file");
	}
	if (!entry) {
		if (!secret_name.empty()) {
			throw BinderException("Secret with name \"%s\" not found", secret_name);
		}
		return nullptr;
	}
	if (entry->secret->GetType() != TYPE_NAME) {
		throw BinderException("Secret \"%s\" is not a ClickHouse secret (it has type \"%s\")", name,
		                      entry->secret->GetType());
	}
	return entry;
}

void ClickhouseSecrets::ApplySecret(const SecretEntry &entry, ClickhouseConnectionConfig &config) {
	auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*entry.secret);
	for (auto &item : kv_secret.secret_map) {
		config.SetOption(item.first, item.second.ToString());
	}
}

} // namespace duckdb
```

- [ ] **Step 5: The connection wrapper**

`src/include/clickhouse_connection.hpp`:
```cpp
#pragma once

#include "clickhouse_connection_config.hpp"
#include "duckdb/common/common.hpp"

#include <atomic>
#include <chrono>
#include <optional>

#include <clickhouse/client.h>

namespace duckdb {
class ClientContext;

struct ClickhouseTimeouts {
	uint64_t connect_timeout_ms = 10000;
	uint64_t receive_timeout_ms = 300000;

	//! Reads ch_connect_timeout_ms / ch_receive_timeout_ms
	static ClickhouseTimeouts FromContext(ClientContext &context);
};

//! One native-protocol connection to ClickHouse. Not thread-safe: callers serialize access.
class ClickhouseConnection {
public:
	ClickhouseConnection(unique_ptr<clickhouse::Client> client, ClickhouseConnectionConfig config);
	~ClickhouseConnection();

	//! Connects. Throws IOException("Failed to connect to ClickHouse at host:port: ...") on failure
	static unique_ptr<ClickhouseConnection> Open(const ClickhouseConnectionConfig &config,
	                                             const ClickhouseTimeouts &timeouts);

	//! Starts a streaming query; read its blocks with NextBlock()
	void BeginQuery(const string &sql);
	//! The next block (possibly with zero rows); nullopt once the query has finished
	std::optional<clickhouse::Block> NextBlock();
	//! Cancels the running query (if any) and drains the connection
	void Cancel();
	bool IsQueryRunning() const;

	//! Runs a query to completion and returns all of its blocks
	vector<clickhouse::Block> Query(const string &sql);

	//! Usable for a new query: not broken, not mid-query, and answers a ping when it has been idle for 30s
	bool IsHealthy();
	//! True after a network or protocol error. Broken connections are never reused
	bool IsBroken() const;

	static void SetDebugPrintQueries(bool print);

private:
	clickhouse::Query MakeQuery(const string &sql) const;
	//! Translates the in-flight exception into a DuckDB exception
	[[noreturn]] void RethrowAsDuckDBException(const string &sql);

	unique_ptr<clickhouse::Client> client;
	ClickhouseConnectionConfig config;
	bool broken = false;
	std::chrono::steady_clock::time_point last_used;

	static std::atomic<bool> debug_print_queries;
};

} // namespace duckdb
```

`src/clickhouse_connection.cpp`:
```cpp
#include "clickhouse_connection.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

#include <fstream>

namespace duckdb {

std::atomic<bool> ClickhouseConnection::debug_print_queries {false};

//! LowCardinality columns are sent as plain columns, so the conversion code never sees dictionary encoding
static constexpr const char *LOW_CARDINALITY_SETTING = "low_cardinality_allow_in_native_format";
static constexpr auto PING_AFTER_IDLE = std::chrono::seconds(30);

ClickhouseTimeouts ClickhouseTimeouts::FromContext(ClientContext &context) {
	ClickhouseTimeouts result;
	Value value;
	if (context.TryGetCurrentSetting("ch_connect_timeout_ms", value) && !value.IsNull()) {
		result.connect_timeout_ms = UBigIntValue::Get(value);
	}
	if (context.TryGetCurrentSetting("ch_receive_timeout_ms", value) && !value.IsNull()) {
		result.receive_timeout_ms = UBigIntValue::Get(value);
	}
	return result;
}

//! vcpkg's OpenSSL does not know where the operating system keeps its CA bundle
static string FindSystemCABundle() {
	static const char *CANDIDATES[] = {"/etc/ssl/certs/ca-certificates.crt",
	                                   "/etc/pki/tls/certs/ca-bundle.crt",
	                                   "/etc/ssl/ca-bundle.pem",
	                                   "/etc/pki/tls/cacert.pem",
	                                   "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
	                                   "/etc/ssl/cert.pem"};
	for (auto candidate : CANDIDATES) {
		std::ifstream file(candidate);
		if (file.good()) {
			return candidate;
		}
	}
	return string();
}

static clickhouse::ClientOptions MakeClientOptions(const ClickhouseConnectionConfig &config,
                                                   const ClickhouseTimeouts &timeouts) {
	clickhouse::ClientOptions options;
	options.SetHost(config.host);
	options.SetPort(config.GetPort());
	options.SetUser(config.user);
	options.SetPassword(config.password);
	options.SetDefaultDatabase(config.database);
	options.SetRethrowException(true);
	options.SetSendRetries(1);
	options.TcpKeepAlive(true);
	options.SetConnectionConnectTimeout(std::chrono::milliseconds(timeouts.connect_timeout_ms));
	options.SetConnectionRecvTimeout(std::chrono::milliseconds(timeouts.receive_timeout_ms));
	if (config.compression == "lz4") {
		options.SetCompressionMethod(clickhouse::CompressionMethod::LZ4);
	} else if (config.compression == "zstd") {
		options.SetCompressionMethod(clickhouse::CompressionMethod::ZSTD);
	} else {
		options.SetCompressionMethod(clickhouse::CompressionMethod::None);
	}
	if (config.IsSecure()) {
		clickhouse::ClientOptions::SSLOptions ssl_options;
		if (!config.ca_cert.empty()) {
			ssl_options.SetPathToCAFiles({config.ca_cert});
			ssl_options.SetUseDefaultCALocations(false);
		} else {
			auto bundle = FindSystemCABundle();
			if (!bundle.empty()) {
				ssl_options.SetPathToCAFiles({bundle});
			}
		}
		ssl_options.SetSkipVerification(config.skip_verify);
		options.SetSSLOptions(std::move(ssl_options));
	}
	return options;
}

ClickhouseConnection::ClickhouseConnection(unique_ptr<clickhouse::Client> client_p, ClickhouseConnectionConfig config_p)
    : client(std::move(client_p)), config(std::move(config_p)), last_used(std::chrono::steady_clock::now()) {
}

ClickhouseConnection::~ClickhouseConnection() = default;

unique_ptr<ClickhouseConnection> ClickhouseConnection::Open(const ClickhouseConnectionConfig &config,
                                                            const ClickhouseTimeouts &timeouts) {
	try {
		auto client = make_uniq<clickhouse::Client>(MakeClientOptions(config, timeouts));
		return make_uniq<ClickhouseConnection>(std::move(client), config);
	} catch (const clickhouse::ServerException &ex) {
		auto &error = ex.GetException();
		throw IOException("Failed to connect to ClickHouse at %s:%d: ClickHouse error %d (%s): %s", config.host,
		                  static_cast<int32_t>(config.GetPort()), error.code, error.name, error.display_text);
	} catch (const std::exception &ex) {
		throw IOException("Failed to connect to ClickHouse at %s:%d: %s", config.host,
		                  static_cast<int32_t>(config.GetPort()), ex.what());
	}
}

void ClickhouseConnection::SetDebugPrintQueries(bool print) {
	debug_print_queries = print;
}

clickhouse::Query ClickhouseConnection::MakeQuery(const string &sql) const {
	clickhouse::Query query(sql);
	query.SetSetting(LOW_CARDINALITY_SETTING, clickhouse::QuerySettingsField {"0", 0});
	for (auto &setting : config.settings) {
		// IMPORTANT makes the server reject unknown settings instead of silently ignoring them
		query.SetSetting(setting.first,
		                 clickhouse::QuerySettingsField {setting.second, clickhouse::QuerySettingsField::IMPORTANT});
	}
	return query;
}

void ClickhouseConnection::RethrowAsDuckDBException(const string &sql) {
	auto query_suffix = debug_print_queries && !sql.empty() ? "\nQuery: " + sql : string();
	try {
		throw;
	} catch (const clickhouse::ServerException &ex) {
		// the server reported an error; the connection itself is still in a clean state
		auto &error = ex.GetException();
		throw IOException("ClickHouse error %d (%s): %s%s", error.code, error.name, error.display_text, query_suffix);
	} catch (const Exception &) {
		broken = true;
		throw;
	} catch (const std::exception &ex) {
		broken = true;
		throw IOException("ClickHouse connection error (%s:%d): %s%s", config.host,
		                  static_cast<int32_t>(config.GetPort()), ex.what(), query_suffix);
	}
}

void ClickhouseConnection::BeginQuery(const string &sql) {
	if (debug_print_queries) {
		Printer::Print(sql + "\n");
	}
	last_used = std::chrono::steady_clock::now();
	try {
		client->BeginSelect(MakeQuery(sql));
	} catch (...) {
		RethrowAsDuckDBException(sql);
	}
}

std::optional<clickhouse::Block> ClickhouseConnection::NextBlock() {
	try {
		auto block = client->NextBlock();
		last_used = std::chrono::steady_clock::now();
		return block;
	} catch (...) {
		RethrowAsDuckDBException(string());
	}
}

void ClickhouseConnection::Cancel() {
	if (!client->IsSelecting()) {
		return;
	}
	try {
		client->Cancel();
	} catch (...) {
		broken = true;
	}
}

bool ClickhouseConnection::IsQueryRunning() const {
	return client->IsSelecting();
}

vector<clickhouse::Block> ClickhouseConnection::Query(const string &sql) {
	vector<clickhouse::Block> result;
	BeginQuery(sql);
	while (true) {
		auto block = NextBlock();
		if (!block) {
			break;
		}
		result.push_back(std::move(*block));
	}
	return result;
}

bool ClickhouseConnection::IsHealthy() {
	if (broken || client->IsSelecting()) {
		return false;
	}
	auto now = std::chrono::steady_clock::now();
	if (now - last_used < PING_AFTER_IDLE) {
		return true;
	}
	try {
		client->Ping();
		last_used = now;
		return true;
	} catch (...) {
		broken = true;
		return false;
	}
}

bool ClickhouseConnection::IsBroken() const {
	return broken;
}

} // namespace duckdb
```

- [ ] **Step 6: The pool and the transaction manager**

`src/include/storage/clickhouse_connection_pool.hpp`:
```cpp
#pragma once

#include "clickhouse_connection.hpp"
#include "dbconnector/pool.hpp"

namespace duckdb {
class ClientContext;

using ClickhousePoolConnection = dbconnector::pool::PooledConnection<ClickhouseConnection>;

class ClickhouseConnectionPool : public dbconnector::pool::ConnectionPool<ClickhouseConnection> {
public:
	ClickhouseConnectionPool(ClickhouseConnectionConfig config, ClickhouseTimeouts timeouts,
	                         dbconnector::pool::ConnectionPoolConfig pool_config);

	ClickhousePoolConnection GetConnection();

	//! Reads the ch_pool_* settings
	static dbconnector::pool::ConnectionPoolConfig PoolConfigFromContext(ClientContext &context);

protected:
	std::unique_ptr<ClickhouseConnection> CreateNewConnection() override;
	bool CheckConnectionHealthy(ClickhouseConnection &connection) override;
	void ResetConnection(ClickhouseConnection &connection) override;

private:
	ClickhouseConnectionConfig config;
	ClickhouseTimeouts timeouts;
};

} // namespace duckdb
```

`src/storage/clickhouse_connection_pool.cpp`:
```cpp
#include "storage/clickhouse_connection_pool.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

ClickhouseConnectionPool::ClickhouseConnectionPool(ClickhouseConnectionConfig config_p, ClickhouseTimeouts timeouts_p,
                                                   dbconnector::pool::ConnectionPoolConfig pool_config)
    : dbconnector::pool::ConnectionPool<ClickhouseConnection>(pool_config), config(std::move(config_p)),
      timeouts(timeouts_p) {
}

ClickhousePoolConnection ClickhouseConnectionPool::GetConnection() {
	return Acquire();
}

std::unique_ptr<ClickhouseConnection> ClickhouseConnectionPool::CreateNewConnection() {
	return ClickhouseConnection::Open(config, timeouts);
}

bool ClickhouseConnectionPool::CheckConnectionHealthy(ClickhouseConnection &connection) {
	return connection.IsHealthy();
}

void ClickhouseConnectionPool::ResetConnection(ClickhouseConnection &connection) {
	if (connection.IsQueryRunning()) {
		connection.Cancel();
	}
}

dbconnector::pool::ConnectionPoolConfig ClickhouseConnectionPool::PoolConfigFromContext(ClientContext &context) {
	dbconnector::pool::ConnectionPoolConfig result;
	Value value;
	if (context.TryGetCurrentSetting("ch_pool_acquire_mode", value) && !value.IsNull()) {
		try {
			result.acquire_mode = dbconnector::pool::AcquireModeHelpers::FromString(value.ToString());
		} catch (std::exception &ex) {
			throw InvalidInputException("Invalid ch_pool_acquire_mode \"%s\": expected force, wait or try",
			                            value.ToString());
		}
	}
	if (context.TryGetCurrentSetting("ch_pool_max_connections", value) && !value.IsNull()) {
		result.max_connections = UBigIntValue::Get(value);
	}
	if (context.TryGetCurrentSetting("ch_pool_wait_timeout_millis", value) && !value.IsNull()) {
		result.wait_timeout_millis = UBigIntValue::Get(value);
	}
	if (context.TryGetCurrentSetting("ch_pool_idle_timeout_millis", value) && !value.IsNull()) {
		result.idle_timeout_millis = UBigIntValue::Get(value);
	}
	return result;
}

} // namespace duckdb
```

`src/include/storage/clickhouse_transaction.hpp`:
```cpp
#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

//! ClickHouse has no multi-statement transactions: a DuckDB transaction on an attached ClickHouse database holds no
//! remote state and every scan is an independent ClickHouse query
class ClickhouseTransaction : public Transaction {
public:
	ClickhouseTransaction(TransactionManager &manager, ClientContext &context);
	~ClickhouseTransaction() override;
};

class ClickhouseTransactionManager : public TransactionManager {
public:
	explicit ClickhouseTransactionManager(AttachedDatabase &db);

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	mutex transaction_lock;
	reference_map_t<Transaction, unique_ptr<ClickhouseTransaction>> transactions;
};

} // namespace duckdb
```

`src/storage/clickhouse_transaction.cpp`:
```cpp
#include "storage/clickhouse_transaction.hpp"

#include "duckdb/common/error_data.hpp"

namespace duckdb {

ClickhouseTransaction::ClickhouseTransaction(TransactionManager &manager, ClientContext &context)
    : Transaction(manager, context) {
}

ClickhouseTransaction::~ClickhouseTransaction() = default;

ClickhouseTransactionManager::ClickhouseTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
}

Transaction &ClickhouseTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<ClickhouseTransaction>(*this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(transaction_lock);
	transactions[result] = std::move(transaction);
	return result;
}

ErrorData ClickhouseTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
	return ErrorData();
}

void ClickhouseTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	transactions.erase(transaction);
}

void ClickhouseTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// nothing to checkpoint: all data lives in ClickHouse
}

} // namespace duckdb
```

- [ ] **Step 7: Catalog set, schema set and schema entry**

`src/include/storage/clickhouse_catalog_set.hpp`:
```cpp
#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <functional>

namespace duckdb {
class Catalog;
class ClientContext;

//! A lazily loaded, cached set of catalog entries (the schemas of a catalog, or the tables of a schema)
class ClickhouseCatalogSet {
public:
	explicit ClickhouseCatalogSet(Catalog &catalog);
	virtual ~ClickhouseCatalogSet() = default;

	//! Exact (case-sensitive) match first, then the first case-insensitive match
	optional_ptr<CatalogEntry> GetEntry(ClientContext &context, const string &name);
	void Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback);
	//! Drops the cache; the next access reloads from ClickHouse
	void ClearEntries();

protected:
	virtual void LoadEntries(ClientContext &context) = 0;
	void CreateEntry(unique_ptr<CatalogEntry> entry);

	Catalog &catalog;

private:
	void TryLoadEntries(ClientContext &context);

	mutex load_lock;
	mutex entry_lock;
	bool is_loaded = false;
	vector<shared_ptr<CatalogEntry>> ordered_entries;
	unordered_map<string, shared_ptr<CatalogEntry>> entries;
};

} // namespace duckdb
```

`src/storage/clickhouse_catalog_set.cpp`:
```cpp
#include "storage/clickhouse_catalog_set.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {

ClickhouseCatalogSet::ClickhouseCatalogSet(Catalog &catalog) : catalog(catalog) {
}

void ClickhouseCatalogSet::TryLoadEntries(ClientContext &context) {
	lock_guard<mutex> load_guard(load_lock);
	if (is_loaded) {
		return;
	}
	try {
		LoadEntries(context);
	} catch (...) {
		lock_guard<mutex> guard(entry_lock);
		entries.clear();
		ordered_entries.clear();
		throw;
	}
	is_loaded = true;
}

optional_ptr<CatalogEntry> ClickhouseCatalogSet::GetEntry(ClientContext &context, const string &name) {
	TryLoadEntries(context);
	lock_guard<mutex> guard(entry_lock);
	auto exact = entries.find(name);
	if (exact != entries.end()) {
		return exact->second.get();
	}
	for (auto &entry : ordered_entries) {
		if (StringUtil::CIEquals(entry->name, name)) {
			return entry.get();
		}
	}
	return nullptr;
}

void ClickhouseCatalogSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	TryLoadEntries(context);
	vector<shared_ptr<CatalogEntry>> snapshot;
	{
		lock_guard<mutex> guard(entry_lock);
		snapshot = ordered_entries;
	}
	for (auto &entry : snapshot) {
		callback(*entry);
	}
}

void ClickhouseCatalogSet::ClearEntries() {
	lock_guard<mutex> load_guard(load_lock);
	lock_guard<mutex> guard(entry_lock);
	entries.clear();
	ordered_entries.clear();
	is_loaded = false;
}

void ClickhouseCatalogSet::CreateEntry(unique_ptr<CatalogEntry> entry) {
	shared_ptr<CatalogEntry> shared_entry(std::move(entry));
	lock_guard<mutex> guard(entry_lock);
	entries[shared_entry->name] = shared_entry;
	ordered_entries.push_back(std::move(shared_entry));
}

} // namespace duckdb
```

`src/include/storage/clickhouse_schema_set.hpp`:
```cpp
#pragma once

#include "storage/clickhouse_catalog_set.hpp"

namespace duckdb {

//! The ClickHouse databases of an attached service, exposed as DuckDB schemas
class ClickhouseSchemaSet : public ClickhouseCatalogSet {
public:
	explicit ClickhouseSchemaSet(Catalog &catalog);

protected:
	void LoadEntries(ClientContext &context) override;
};

} // namespace duckdb
```

`src/storage/clickhouse_schema_set.cpp`:
```cpp
#include "storage/clickhouse_schema_set.hpp"

#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_schema_entry.hpp"

namespace duckdb {

ClickhouseSchemaSet::ClickhouseSchemaSet(Catalog &catalog) : ClickhouseCatalogSet(catalog) {
}

static bool IsSystemDatabase(const string &name) {
	return name == "system" || name == "INFORMATION_SCHEMA" || name == "information_schema";
}

void ClickhouseSchemaSet::LoadEntries(ClientContext &context) {
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	auto show_system = ch_catalog.GetAttachOptions().show_system;
	auto &default_database = ch_catalog.GetConfig().database;
	auto connection = ch_catalog.GetConnectionPool().GetConnection();
	auto blocks = connection->Query("SELECT name FROM system.databases ORDER BY name");
	for (auto &block : blocks) {
		auto names = block[0]->As<clickhouse::ColumnString>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			string name(names->At(row));
			if (IsSystemDatabase(name) && !show_system && name != default_database) {
				continue;
			}
			CreateSchemaInfo info;
			info.schema = name;
			info.internal = false;
			CreateEntry(make_uniq<ClickhouseSchemaEntry>(catalog, info));
		}
	}
}

} // namespace duckdb
```

`src/include/storage/clickhouse_schema_entry.hpp`:
```cpp
#pragma once

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {

class ClickhouseSchemaEntry : public SchemaCatalogEntry {
public:
	ClickhouseSchemaEntry(Catalog &catalog, CreateSchemaInfo &info);

	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;
	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;
};

} // namespace duckdb
```

`src/storage/clickhouse_schema_entry.cpp`:
```cpp
#include "storage/clickhouse_schema_entry.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

ClickhouseSchemaEntry::ClickhouseSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info) {
}

optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &,
                                                              TableCatalogEntry &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreatePragmaFunction(CatalogTransaction,
                                                                       CreatePragmaFunctionInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
optional_ptr<CatalogEntry> ClickhouseSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
void ClickhouseSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}
void ClickhouseSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	ClickhouseUtils::ThrowReadOnly();
}

void ClickhouseSchemaEntry::Scan(ClientContext &context, CatalogType type,
                                 const std::function<void(CatalogEntry &)> &callback) {
	// tables are added in Task 5
}

void ClickhouseSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	throw NotImplementedException("Scanning a ClickHouse schema requires a client context");
}

optional_ptr<CatalogEntry> ClickhouseSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                              const EntryLookupInfo &lookup_info) {
	// tables are added in Task 5
	return nullptr;
}

} // namespace duckdb
```

- [ ] **Step 8: Catalog and storage extension**

`src/include/storage/clickhouse_catalog.hpp`:
```cpp
#pragma once

#include "clickhouse_connection_config.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "storage/clickhouse_connection_pool.hpp"
#include "storage/clickhouse_schema_set.hpp"

namespace duckdb {

struct ClickhouseAttachOptions {
	//! Also expose system, INFORMATION_SCHEMA and information_schema
	bool show_system = false;
};

class ClickhouseCatalog : public Catalog {
public:
	ClickhouseCatalog(AttachedDatabase &db, ClickhouseConnectionConfig config, ClickhouseAttachOptions options,
	                  ClientContext &context);
	~ClickhouseCatalog() override;

	static constexpr const char *CATALOG_TYPE = "clickhouse";

	const ClickhouseConnectionConfig &GetConfig() const {
		return config;
	}
	const ClickhouseAttachOptions &GetAttachOptions() const {
		return options;
	}
	ClickhouseConnectionPool &GetConnectionPool() {
		return *connection_pool;
	}
	shared_ptr<ClickhouseConnectionPool> GetConnectionPoolPtr() {
		return connection_pool;
	}
	//! Forgets all cached databases, tables and columns
	void ClearCache();

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return CATALOG_TYPE;
	}
	string GetDefaultSchema() const override {
		return config.database;
	}
	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override {
		return config.ToDisplayString();
	}

private:
	void DropSchema(ClientContext &context, DropInfo &info) override;

	ClickhouseConnectionConfig config;
	ClickhouseAttachOptions options;
	shared_ptr<ClickhouseConnectionPool> connection_pool;
	ClickhouseSchemaSet schemas;
};

} // namespace duckdb
```

`src/storage/clickhouse_catalog.cpp`:
```cpp
#include "storage/clickhouse_catalog.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/storage/database_size.hpp"
#include "storage/clickhouse_schema_entry.hpp"

namespace duckdb {

ClickhouseCatalog::ClickhouseCatalog(AttachedDatabase &db, ClickhouseConnectionConfig config_p,
                                     ClickhouseAttachOptions options_p, ClientContext &context)
    : Catalog(db), config(std::move(config_p)), options(options_p),
      connection_pool(make_shared_ptr<ClickhouseConnectionPool>(
          config, ClickhouseTimeouts::FromContext(context), ClickhouseConnectionPool::PoolConfigFromContext(context))),
      schemas(*this) {
	// connect now so that a wrong host, port, certificate or password fails the ATTACH itself
	auto connection = connection_pool->GetConnection();
}

ClickhouseCatalog::~ClickhouseCatalog() = default;

void ClickhouseCatalog::Initialize(bool load_builtin) {
}

void ClickhouseCatalog::ClearCache() {
	schemas.ClearEntries();
}

optional_ptr<CatalogEntry> ClickhouseCatalog::CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) {
	ClickhouseUtils::ThrowReadOnly();
}

void ClickhouseCatalog::DropSchema(ClientContext &context, DropInfo &info) {
	ClickhouseUtils::ThrowReadOnly();
}

void ClickhouseCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	schemas.Scan(context, [&](CatalogEntry &schema) { callback(schema.Cast<SchemaCatalogEntry>()); });
}

optional_ptr<SchemaCatalogEntry> ClickhouseCatalog::LookupSchema(CatalogTransaction transaction,
                                                                 const EntryLookupInfo &schema_lookup,
                                                                 OnEntryNotFound if_not_found) {
	auto &context = transaction.GetContext();
	auto &schema_name = schema_lookup.GetEntryName();
	auto entry = schemas.GetEntry(context, schema_name);
	if (!entry && schema_name == DEFAULT_SCHEMA) {
		// "main" refers to the database named in the connection settings
		entry = schemas.GetEntry(context, config.database);
	}
	if (!entry) {
		if (if_not_found == OnEntryNotFound::RETURN_NULL) {
			return nullptr;
		}
		throw BinderException("ClickHouse database \"%s\" not found", schema_name);
	}
	return &entry->Cast<SchemaCatalogEntry>();
}

PhysicalOperator &ClickhouseCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                       PhysicalOperator &) {
	ClickhouseUtils::ThrowReadOnly();
}

PhysicalOperator &ClickhouseCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
                                                optional_ptr<PhysicalOperator>) {
	ClickhouseUtils::ThrowReadOnly();
}

PhysicalOperator &ClickhouseCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
                                                PhysicalOperator &) {
	ClickhouseUtils::ThrowReadOnly();
}

PhysicalOperator &ClickhouseCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
                                                PhysicalOperator &) {
	ClickhouseUtils::ThrowReadOnly();
}

DatabaseSize ClickhouseCatalog::GetDatabaseSize(ClientContext &context) {
	return DatabaseSize();
}

} // namespace duckdb
```

`src/include/storage/clickhouse_storage_extension.hpp`:
```cpp
#pragma once

#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

class ClickhouseStorageExtension : public StorageExtension {
public:
	ClickhouseStorageExtension();
};

} // namespace duckdb
```

`src/storage/clickhouse_storage_extension.cpp`:
```cpp
#include "storage/clickhouse_storage_extension.hpp"

#include "clickhouse_secrets.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_transaction.hpp"

namespace duckdb {

static unique_ptr<Catalog> ClickhouseAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                                            AttachedDatabase &db, const string &name, AttachInfo &info,
                                            AttachOptions &attach_options) {
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("Attaching ClickHouse databases is disabled through configuration");
	}
	string secret_name;
	string settings;
	ClickhouseAttachOptions options;
	for (auto &entry : attach_options.options) {
		auto key = StringUtil::Lower(entry.first);
		if (key == "secret") {
			secret_name = entry.second.ToString();
		} else if (key == "settings") {
			settings = entry.second.ToString();
		} else if (key == "show_system") {
			options.show_system = BooleanValue::Get(entry.second.DefaultCastAs(LogicalType::BOOLEAN));
		} else {
			throw BinderException("Unrecognized option for ClickHouse attach: %s", entry.first);
		}
	}
	ClickhouseConnectionConfig config;
	auto secret_entry = ClickhouseSecrets::GetSecretEntry(context, secret_name);
	if (secret_entry) {
		ClickhouseSecrets::ApplySecret(*secret_entry, config);
	}
	config.ApplyConnectionString(info.path);
	if (!settings.empty()) {
		config.AddSettings(settings);
	}
	return make_uniq<ClickhouseCatalog>(db, std::move(config), options, context);
}

static unique_ptr<TransactionManager> ClickhouseCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                                         AttachedDatabase &db, Catalog &catalog) {
	return make_uniq<ClickhouseTransactionManager>(db);
}

ClickhouseStorageExtension::ClickhouseStorageExtension() {
	attach = ClickhouseAttach;
	create_transaction_manager = ClickhouseCreateTransactionManager;
}

} // namespace duckdb
```

- [ ] **Step 9: Register secrets, storage and settings; update CMake**

In `src/clickhouse_scanner_extension.cpp` add these includes:
```cpp
#include "clickhouse_connection.hpp"
#include "clickhouse_secrets.hpp"
#include "dbconnector/pool.hpp"
#include "duckdb/main/config.hpp"
#include "storage/clickhouse_storage_extension.hpp"
```
Add this function above `LoadInternal`:
```cpp
static void SetClickhouseDebugPrintQueries(ClientContext &context, SetScope scope, Value &parameter) {
	ClickhouseConnection::SetDebugPrintQueries(BooleanValue::Get(parameter));
}
```
Append to the end of `LoadInternal`:
```cpp
	loader.RegisterSecretType(ClickhouseSecrets::CreateType());
	CreateSecretFunction secret_function = {ClickhouseSecrets::TYPE_NAME, "config", ClickhouseSecrets::CreateFunction};
	ClickhouseSecrets::SetSecretParameters(secret_function);
	loader.RegisterFunction(secret_function);

	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	auto storage_extension = make_shared_ptr<ClickhouseStorageExtension>();
	StorageExtension::Register(config, "clickhouse_scanner", storage_extension);
	StorageExtension::Register(config, "clickhouse", storage_extension);

	dbconnector::pool::ConnectionPoolConfig default_pool_config;
	config.AddExtensionOption("ch_debug_show_queries", "DEBUG SETTING: print all queries sent to ClickHouse to stdout",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), SetClickhouseDebugPrintQueries);
	config.AddExtensionOption("ch_connect_timeout_ms", "Timeout in milliseconds for connecting to ClickHouse",
	                          LogicalType::UBIGINT, Value::UBIGINT(10000));
	config.AddExtensionOption("ch_receive_timeout_ms", "Timeout in milliseconds for receiving data from ClickHouse",
	                          LogicalType::UBIGINT, Value::UBIGINT(300000));
	config.AddExtensionOption("ch_pool_max_connections",
	                          "Maximum number of pooled connections per attached ClickHouse database (new ATTACHes)",
	                          LogicalType::UBIGINT, Value::UBIGINT(default_pool_config.max_connections));
	config.AddExtensionOption("ch_pool_acquire_mode",
	                          "What to do when the pool is exhausted: force, wait or try (new ATTACHes)",
	                          LogicalType::VARCHAR, Value("force"));
	config.AddExtensionOption("ch_pool_wait_timeout_millis",
	                          "How long 'wait' acquire mode waits for a free connection (new ATTACHes)",
	                          LogicalType::UBIGINT, Value::UBIGINT(default_pool_config.wait_timeout_millis));
	config.AddExtensionOption("ch_pool_idle_timeout_millis",
	                          "Idle pooled connections are closed after this many milliseconds (new ATTACHes)",
	                          LogicalType::UBIGINT, Value::UBIGINT(default_pool_config.idle_timeout_millis));
```

`src/CMakeLists.txt`:
```cmake
add_subdirectory(storage)

add_library(
  clickhouse_ext OBJECT
  clickhouse_connection.cpp
  clickhouse_connection_config.cpp
  clickhouse_scanner_extension.cpp
  clickhouse_secrets.cpp
  clickhouse_type_mapping_function.cpp
  clickhouse_types.cpp
  clickhouse_utils.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:clickhouse_ext>
    PARENT_SCOPE)
```

`src/storage/CMakeLists.txt`:
```cmake
add_library(
  clickhouse_ext_storage OBJECT
  clickhouse_catalog.cpp
  clickhouse_catalog_set.cpp
  clickhouse_connection_pool.cpp
  clickhouse_schema_entry.cpp
  clickhouse_schema_set.cpp
  clickhouse_storage_extension.cpp
  clickhouse_transaction.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:clickhouse_ext_storage>
    PARENT_SCOPE)
```

- [ ] **Step 10: Run the tests to verify they pass**

Run: `make release && make smoke`
Expected: `All tests passed`. Also run `./build/release/test/unittest "test/*"` directly: the server tests are skipped and the rest pass.

- [ ] **Step 11: Commit**

```bash
git add src test
git commit -m "feat: ATTACH ClickHouse over the native protocol with secrets, TLS and pooling

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---
### Task 5: Tables, columns, clear cache and read-only DDL

**Files:**
- Create: `src/include/storage/clickhouse_table_set.hpp`, `src/storage/clickhouse_table_set.cpp`
- Create: `src/include/storage/clickhouse_table_entry.hpp`, `src/storage/clickhouse_table_entry.cpp`
- Create: `src/include/storage/clickhouse_clear_cache.hpp`, `src/storage/clickhouse_clear_cache.cpp`
- Modify: `src/include/storage/clickhouse_schema_entry.hpp`, `src/storage/clickhouse_schema_entry.cpp`, `src/storage/CMakeLists.txt`, `src/clickhouse_scanner_extension.cpp`
- Test: `test/sql/catalog/tables.test`, `test/sql/catalog/columns.test`, `test/sql/catalog/read_only.test`

**Interfaces:**
- Consumes: `ClickhouseCatalogSet`, `ClickhouseCatalog::GetConnectionPool()`, `ClickhouseColumnInfo::Create`, `ClickhouseTypes::IsNullable`, `ClickhouseUtils::QuoteLiteral`.
- Produces:
  - `class ClickhouseTableEntry : public TableCatalogEntry` with `const vector<ClickhouseColumnInfo> &GetClickhouseColumns() const` and `optional_idx GetApproxRows() const`. Task 6 replaces its `GetScanFunction`.
  - `class ClickhouseClearCacheFunction : public TableFunction` (`clickhouse_clear_cache()`) with `static void ClearClickhouseCaches(ClientContext &context)`.

- [ ] **Step 1: Write the failing tests**

`test/sql/catalog/tables.test`:
```
# name: test/sql/catalog/tables.test
# description: ClickHouse tables and views are listed in the DuckDB catalog
# group: [catalog]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

query I
SELECT table_name FROM duckdb_tables() WHERE database_name = 'ch' AND schema_name = 'test_db' ORDER BY table_name;
----
MixedCase
aggregates
bad_times
big
binary
empty
nested
nullables
scalars
semi
t1
times
v1

query II
SELECT schema_name, table_name FROM duckdb_tables() WHERE database_name = 'ch' AND schema_name <> 'test_db' ORDER BY ALL;
----
other_db	other_table

# approximate row counts come from system.tables
query I
SELECT estimated_size FROM duckdb_tables() WHERE database_name = 'ch' AND table_name = 'big';
----
10000000

statement ok
CALL clickhouse_clear_cache();

query I
SELECT count(*) FROM duckdb_tables() WHERE database_name = 'ch' AND schema_name = 'test_db';
----
13
```

`test/sql/catalog/columns.test`:
```
# name: test/sql/catalog/columns.test
# description: ClickHouse column types map to the expected DuckDB types and nullability
# group: [catalog]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

query III
SELECT column_name, data_type, is_nullable FROM duckdb_columns() WHERE database_name = 'ch' AND table_name = 'scalars' ORDER BY column_index;
----
id	UTINYINT	false
b	BOOLEAN	false
i8	TINYINT	false
i16	SMALLINT	false
i32	INTEGER	false
i64	BIGINT	false
u8	UTINYINT	false
u16	USMALLINT	false
u32	UINTEGER	false
u64	UBIGINT	false
i128	HUGEINT	false
u128	UHUGEINT	false
i256	VARCHAR	false
u256	VARCHAR	false
f32	FLOAT	false
f64	DOUBLE	false
d9	DECIMAL(9,2)	false
d18	DECIMAL(18,4)	false
d38	DECIMAL(38,10)	false
d76	VARCHAR	false
s	VARCHAR	false
fs	VARCHAR	false
d	DATE	false
d32	DATE	false
dt	TIMESTAMP WITH TIME ZONE	false
dt64_3	TIMESTAMP WITH TIME ZONE	false
dt64_9	TIMESTAMP WITH TIME ZONE	false
uuid	UUID	false
ip4	VARCHAR	false
ip6	VARCHAR	false
e8	ENUM('blue', 'red', 'green')	false
e16	ENUM('small', 'large')	false

query III
SELECT column_name, data_type, is_nullable FROM duckdb_columns() WHERE database_name = 'ch' AND table_name = 'nullables' ORDER BY column_index;
----
id	UTINYINT	false
i	INTEGER	true
s	VARCHAR	true
d	DATE	true
dt	TIMESTAMP WITH TIME ZONE	true
e	ENUM('a')	true
lc	VARCHAR	true
lcs	VARCHAR	false
dec	DECIMAL(10,2)	true
ip	VARCHAR	true

query II
SELECT column_name, data_type FROM duckdb_columns() WHERE database_name = 'ch' AND table_name = 'nested' ORDER BY column_index;
----
id	UTINYINT
arr	INTEGER[]
arr_null	VARCHAR[]
arr2	UTINYINT[][]
tup	STRUCT(a INTEGER, b VARCHAR)
tup_unnamed	STRUCT("1" INTEGER, "2" VARCHAR)
m	MAP(VARCHAR, UBIGINT)
m_ip	MAP(VARCHAR, VARCHAR)
arr_ip	VARCHAR[]
tup_ip	STRUCT(ip VARCHAR, n TINYINT)

query II
SELECT column_name, data_type FROM duckdb_columns() WHERE database_name = 'ch' AND table_name IN ('semi', 'times', 'aggregates') ORDER BY table_name, column_index;
----
k	UTINYINT
total	VARCHAR
last	VARCHAR
id	UTINYINT
j	JSON
v	JSON
dyn	JSON
id	UTINYINT
t	TIME
t3	TIME
t9	TIME_NS

query II
SELECT column_name, data_type FROM duckdb_columns() WHERE database_name = 'ch' AND table_name = 'v1' ORDER BY column_index;
----
id	UBIGINT
name	VARCHAR
```

`test/sql/catalog/read_only.test`:
```
# name: test/sql/catalog/read_only.test
# description: DDL against an attached ClickHouse database is rejected
# group: [catalog]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

statement error
CREATE TABLE ch.test_db.new_table (i INTEGER);
----
clickhouse_scanner is read-only

statement error
CREATE VIEW ch.test_db.new_view AS SELECT 42;
----
clickhouse_scanner is read-only

statement error
DROP TABLE ch.test_db.t1;
----
clickhouse_scanner is read-only

statement error
ALTER TABLE ch.test_db.t1 ADD COLUMN extra INTEGER;
----
clickhouse_scanner is read-only
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make release && make smoke ARGS='test/sql/catalog/*'`
Expected: FAIL. `duckdb_tables()` returns no ClickHouse rows and `clickhouse_clear_cache` does not exist.

- [ ] **Step 3: Table entry**

`src/include/storage/clickhouse_table_entry.hpp`:
```cpp
#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

class ClickhouseTableEntry : public TableCatalogEntry {
public:
	ClickhouseTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
	                     vector<ClickhouseColumnInfo> columns, optional_idx approx_rows);

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	const vector<ClickhouseColumnInfo> &GetClickhouseColumns() const {
		return clickhouse_columns;
	}
	optional_idx GetApproxRows() const {
		return approx_rows;
	}

private:
	vector<ClickhouseColumnInfo> clickhouse_columns;
	//! system.tables.total_rows (not known for views and some engines)
	optional_idx approx_rows;
};

} // namespace duckdb
```

`src/storage/clickhouse_table_entry.cpp`:
```cpp
#include "storage/clickhouse_table_entry.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/storage/table_storage_info.hpp"

namespace duckdb {

ClickhouseTableEntry::ClickhouseTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info,
                                           vector<ClickhouseColumnInfo> columns, optional_idx approx_rows)
    : TableCatalogEntry(catalog, schema, info), clickhouse_columns(std::move(columns)), approx_rows(approx_rows) {
}

unique_ptr<BaseStatistics> ClickhouseTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	return nullptr;
}

TableFunction ClickhouseTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	// replaced in Task 6
	throw NotImplementedException("Scanning ClickHouse tables is not implemented yet");
}

TableStorageInfo ClickhouseTableEntry::GetStorageInfo(ClientContext &context) {
	TableStorageInfo result;
	if (approx_rows.IsValid()) {
		result.cardinality = approx_rows.GetIndex();
	}
	return result;
}

} // namespace duckdb
```
`duckdb_tables().estimated_size` reads `TableStorageInfo::cardinality`. Check that the field is named `cardinality` in `duckdb/storage/table_storage_info.hpp`. It is an `optional_idx` in v1.5, so assigning an `idx_t` works.

- [ ] **Step 4: Table set**

`src/include/storage/clickhouse_table_set.hpp`:
```cpp
#pragma once

#include "storage/clickhouse_catalog_set.hpp"

namespace duckdb {
class SchemaCatalogEntry;

//! The tables and views of one ClickHouse database
class ClickhouseTableSet : public ClickhouseCatalogSet {
public:
	ClickhouseTableSet(SchemaCatalogEntry &schema, Catalog &catalog);

protected:
	void LoadEntries(ClientContext &context) override;

private:
	SchemaCatalogEntry &schema;
};

} // namespace duckdb
```

`src/storage/clickhouse_table_set.cpp`:
```cpp
#include "storage/clickhouse_table_set.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/parser/constraints/not_null_constraint.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "storage/clickhouse_catalog.hpp"
#include "storage/clickhouse_table_entry.hpp"

namespace duckdb {

ClickhouseTableSet::ClickhouseTableSet(SchemaCatalogEntry &schema, Catalog &catalog)
    : ClickhouseCatalogSet(catalog), schema(schema) {
}

struct ClickhouseTableDefinition {
	string name;
	vector<ClickhouseColumnInfo> columns;
};

void ClickhouseTableSet::LoadEntries(ClientContext &context) {
	auto &ch_catalog = catalog.Cast<ClickhouseCatalog>();
	auto connection = ch_catalog.GetConnectionPool().GetConnection();
	auto database = ClickhouseUtils::QuoteLiteral(schema.name);

	unordered_map<string, idx_t> row_counts;
	for (auto &block :
	     connection->Query("SELECT name, total_rows FROM system.tables WHERE database = " + database)) {
		auto names = block[0]->As<clickhouse::ColumnString>();
		auto totals = block[1]->As<clickhouse::ColumnNullable>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			if (!totals->IsNull(row)) {
				row_counts[string(names->At(row))] =
				    totals->Nested()->As<clickhouse::ColumnUInt64>()->At(row);
			}
		}
	}

	// EPHEMERAL columns only exist for INSERTs and cannot be selected
	auto columns_query = "SELECT table, name, type FROM system.columns WHERE database = " + database +
	                     " AND default_kind != 'EPHEMERAL' ORDER BY table, position";
	vector<ClickhouseTableDefinition> tables;
	for (auto &block : connection->Query(columns_query)) {
		auto table_names = block[0]->As<clickhouse::ColumnString>();
		auto column_names = block[1]->As<clickhouse::ColumnString>();
		auto column_types = block[2]->As<clickhouse::ColumnString>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			string table_name(table_names->At(row));
			if (tables.empty() || tables.back().name != table_name) {
				tables.push_back(ClickhouseTableDefinition {table_name, {}});
			}
			tables.back().columns.push_back(
			    ClickhouseColumnInfo::Create(string(column_names->At(row)), string(column_types->At(row))));
		}
	}

	for (auto &table : tables) {
		CreateTableInfo info(schema, table.name);
		for (idx_t i = 0; i < table.columns.size(); i++) {
			auto &column = table.columns[i];
			info.columns.AddColumn(ColumnDefinition(column.name, column.type));
			if (!ClickhouseTypes::IsNullable(column.type_node)) {
				info.constraints.push_back(make_uniq<NotNullConstraint>(LogicalIndex(i)));
			}
		}
		optional_idx approx_rows;
		auto row_count = row_counts.find(table.name);
		if (row_count != row_counts.end()) {
			approx_rows = row_count->second;
		}
		CreateEntry(
		    make_uniq<ClickhouseTableEntry>(catalog, schema, info, std::move(table.columns), approx_rows));
	}
}

} // namespace duckdb
```

- [ ] **Step 5: Wire tables into the schema entry**

In `src/include/storage/clickhouse_schema_entry.hpp` add `#include "storage/clickhouse_table_set.hpp"` and a private member after the public methods:
```cpp
private:
	ClickhouseTableSet tables;
```

In `src/storage/clickhouse_schema_entry.cpp`, change the constructor, and add the include `#include "duckdb/catalog/catalog_transaction.hpp"`:
```cpp
ClickhouseSchemaEntry::ClickhouseSchemaEntry(Catalog &catalog, CreateSchemaInfo &info)
    : SchemaCatalogEntry(catalog, info), tables(*this, catalog) {
}
```
Replace the two methods marked "tables are added in Task 5":
```cpp
void ClickhouseSchemaEntry::Scan(ClientContext &context, CatalogType type,
                                 const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	tables.Scan(context, callback);
}

optional_ptr<CatalogEntry> ClickhouseSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                              const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	return tables.GetEntry(transaction.GetContext(), lookup_info.GetEntryName());
}
```

- [ ] **Step 6: `clickhouse_clear_cache()`**

`src/include/storage/clickhouse_clear_cache.hpp`:
```cpp
#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ClickhouseClearCacheFunction : public TableFunction {
public:
	ClickhouseClearCacheFunction();

	//! Drops the cached databases, tables and columns of every attached ClickHouse database
	static void ClearClickhouseCaches(ClientContext &context);
};

} // namespace duckdb
```

`src/storage/clickhouse_clear_cache.cpp`:
```cpp
#include "storage/clickhouse_clear_cache.hpp"

#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "storage/clickhouse_catalog.hpp"

namespace duckdb {

struct ClickhouseClearCacheData : public TableFunctionData {
	bool finished = false;
};

static unique_ptr<FunctionData> ClearCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	return_types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("Success");
	return make_uniq<ClickhouseClearCacheData>();
}

void ClickhouseClearCacheFunction::ClearClickhouseCaches(ClientContext &context) {
	auto databases = DatabaseManager::Get(context).GetDatabases(context);
	for (auto &database : databases) {
		auto &catalog = database->GetCatalog();
		if (catalog.GetCatalogType() != ClickhouseCatalog::CATALOG_TYPE) {
			continue;
		}
		catalog.Cast<ClickhouseCatalog>().ClearCache();
	}
}

static void ClearCacheFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<ClickhouseClearCacheData>();
	if (data.finished) {
		return;
	}
	ClickhouseClearCacheFunction::ClearClickhouseCaches(context);
	data.finished = true;
}

ClickhouseClearCacheFunction::ClickhouseClearCacheFunction()
    : TableFunction("clickhouse_clear_cache", {}, ClearCacheFunction, ClearCacheBind) {
}

} // namespace duckdb
```

Register it in `src/clickhouse_scanner_extension.cpp`. Add `#include "storage/clickhouse_clear_cache.hpp"` and, in `LoadInternal` after the type-mapping function:
```cpp
	loader.RegisterFunction(ClickhouseClearCacheFunction());
```

`src/storage/CMakeLists.txt`: add `clickhouse_clear_cache.cpp`, `clickhouse_table_entry.cpp` and `clickhouse_table_set.cpp` to `clickhouse_ext_storage`, keeping the list alphabetical.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `make release && make smoke`
Expected: `All tests passed`. If `ALTER TABLE` fails with a DuckDB binder message before reaching `Alter()`, accept that message in the test, provided it does not modify ClickHouse. If `estimated_size` is `NULL`, check the `TableStorageInfo` field name.

- [ ] **Step 8: Commit**

```bash
git add src test
git commit -m "feat: expose ClickHouse tables, views and columns in the DuckDB catalog

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---
### Task 6: Column conversion and the parallel, cancellable scan

**Files:**
- Create: `src/include/clickhouse_conversion.hpp`, `src/clickhouse_conversion.cpp`
- Create: `src/include/clickhouse_scanner.hpp`, `src/clickhouse_scanner.cpp`
- Modify: `src/storage/clickhouse_table_entry.cpp` (`GetScanFunction`), `src/CMakeLists.txt`
- Test: `test/sql/scan/basic.test`, `test/sql/scan/scalars.test`, `test/sql/scan/nullables.test`, `test/sql/scan/nested.test`, `test/sql/scan/semi_structured.test`, `test/sql/scan/times.test`, `test/sql/scan/errors.test`, `test/sql/scan/read_only.test`

**Interfaces:**
- Consumes: `ClickhouseConnection::{BeginQuery, NextBlock, Cancel, IsQueryRunning}`, `ClickhouseConnectionPool::GetConnection`, `ClickhouseColumnInfo`, `ClickhouseTypes::ReadExpression`, `ClickhouseUtils::{QuoteIdentifier, IsValidUtf8}`.
- Produces:
  - `ClickhouseConversion::ConvertBlock(const clickhouse::Block &, DataChunk &, idx_t offset, idx_t count, const vector<string> &column_names)`.
  - `ClickhouseConversion::ConvertColumn(const clickhouse::ColumnRef &, Vector &, idx_t offset, idx_t count, const string &column_name)`.
  - `struct ClickhouseScanBindData : TableFunctionData { shared_ptr<ClickhouseConnectionPool> pool; string database, table, query; vector<ClickhouseColumnInfo> columns; optional_idx approx_rows; bool filter_pushdown; string order_by_clause, limit_clause; }`. The optimizer (Task 8) writes `order_by_clause` and `limit_clause`, for example `" ORDER BY `n` DESC NULLS LAST"` and `" LIMIT 3"`.
  - `class ClickhouseScanFunction : TableFunction` (name `clickhouse_scan`) with static `SetScanCallbacks(TableFunction &)`, `BuildQuery(const ClickhouseScanBindData &, const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters) -> string`, `SetReturnTypes(const ClickhouseScanBindData &, vector<LogicalType> &, vector<string> &)` and `IsClickhouseScan(const string &function_name) -> bool`.
  - `EXPLAIN ANALYZE` shows the generated SQL under the key `ClickHouse Query`.

- [ ] **Step 1: Write the failing tests**

Every server test below starts with this header. It is shown once here and written out in full in every file:
```
require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

require icu

statement ok
SET TimeZone = 'UTC';

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);
```

`test/sql/scan/basic.test` (the header is preceded by `# name: test/sql/scan/basic.test`, `# description: basic scans, projections, parallelism and early termination`, `# group: [scan]`):
```
query IITT
SELECT id, name, value, created_at FROM ch.test_db.t1 ORDER BY id;
----
1	Alice	99.5	2024-01-01 00:00:00+00
2	Bob	150.25	2024-01-02 00:00:00+00
3	Charlie	200.0	2024-01-03 00:00:00+00

query II
SELECT name, id FROM ch.test_db.t1 ORDER BY id;
----
Alice	1
Bob	2
Charlie	3

query II
SELECT * FROM ch.test_db.v1 ORDER BY id;
----
1	ALICE
2	BOB
3	CHARLIE

query I
SELECT count(*) FROM ch.test_db.empty;
----
0

query I
SELECT count(*) FROM ch.test_db.t1;
----
3

# table lookups fall back to a case-insensitive match
query I
SELECT id FROM ch.test_db.mixedcase;
----
7

query I
SELECT "Id" FROM ch.test_db."MixedCase";
----
7

query I
SELECT x FROM ch.other_db.other_table;
----
1

# the default schema is the database from the connection string
statement ok
USE ch;

query I
SELECT count(*) FROM t1;
----
3

statement ok
USE memory;

statement ok
SET threads = 4;

query II
SELECT count(*), sum(n) FROM ch.test_db.big;
----
10000000	49999995000000

query I
SELECT count(*) FROM ch.test_db.t1 a JOIN ch.test_db.t1 b ON a.id = b.id;
----
3

# stopping a scan early cancels the ClickHouse query and returns a clean connection to the pool
# (the expression filter keeps the LIMIT in DuckDB even once LIMIT pushdown exists)
loop i 0 20

query I
SELECT count(*) FROM (SELECT n FROM ch.test_db.big WHERE n % 2 = 0 LIMIT 10);
----
10

endloop

query I
SELECT count(*) FROM ch.test_db.t1;
----
3

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT name FROM ch.test_db.t1;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT `name` FROM `test_db`.`t1`".*
```

`test/sql/scan/scalars.test` (`# name: test/sql/scan/scalars.test`, `# description: scalar ClickHouse types are read with the right values`, `# group: [scan]`, then the header):
```
query IIIIIIIII
SELECT id, b, i8, i16, i32, i64, u8, u16, u32 FROM ch.test_db.scalars ORDER BY id;
----
1	true	-128	-32768	-2147483648	-9223372036854775808	255	65535	4294967295
2	false	0	0	0	0	0	0	0

query IIIIII
SELECT id, u64, i128, u128, i256, u256 FROM ch.test_db.scalars ORDER BY id;
----
1	18446744073709551615	-170141183460469231731687303715884105728	340282366920938463463374607431768211455	-1	1
2	0	0	0	0	0

query IIIIIII
SELECT id, f32, f64, d9, d18, d38, d76 FROM ch.test_db.scalars ORDER BY id;
----
1	1.5	-2.25	1234567.89	12345678901234.5678	1234567890123456789012345678.0123456789	12345.67891
2	0.0	0.0	0.00	0.0000	0.0000000000	0

query IIIIIIII
SELECT id, s, fs, d, d32, dt, dt64_3, dt64_9 FROM ch.test_db.scalars ORDER BY id;
----
1	hello	abc	2024-02-29	1900-01-01	2024-02-29 12:34:56+00	2024-02-29 12:34:56.789+00	2024-02-29 12:34:56.123456+00
2	(empty)	xyz	1970-01-01	1970-01-01	1970-01-01 00:00:00+00	1970-01-01 00:00:00+00	1970-01-01 00:00:00+00

query IIIIII
SELECT id, uuid, ip4, ip6, e8, e16 FROM ch.test_db.scalars ORDER BY id;
----
1	61f0c404-5cb3-11e7-907b-a6006ad3dba0	192.168.0.1	2001:db8::1	blue	large
2	00000000-0000-0000-0000-000000000000	0.0.0.0	::	red	small
```

`test/sql/scan/nullables.test` (`# name: test/sql/scan/nullables.test`, `# description: NULLs and LowCardinality columns`, `# group: [scan]`, then the header):
```
query IIIIIIIIII
SELECT * FROM ch.test_db.nullables ORDER BY id;
----
1	NULL	NULL	NULL	NULL	NULL	NULL	x	NULL	NULL
2	42	text	2024-01-01	2024-01-01 00:00:00+00	a	lc	y	3.14	10.0.0.1
```

`test/sql/scan/nested.test` (`# name: test/sql/scan/nested.test`, `# description: Array, Tuple and Map columns`, `# group: [scan]`, then the header):
```
query IIIIIIIIII
SELECT * FROM ch.test_db.nested ORDER BY id;
----
1	[1, 2, 3]	[a, NULL]	[[1], [2, 3]]	{'a': 1, 'b': x}	{'1': 2, '2': y}	{k1=1, k2=2}	{h=1.2.3.4}	[1.1.1.1, 2.2.2.2]	{'ip': 8.8.8.8, 'n': 5}
2	[]	[]	[]	{'a': 0, 'b': }	{'1': 0, '2': }	{}	{}	[]	{'ip': 0.0.0.0, 'n': 0}

query I
SELECT m['k2'] FROM ch.test_db.nested WHERE id = 1;
----
2
```

`test/sql/scan/semi_structured.test` (`# name: test/sql/scan/semi_structured.test`, `# description: JSON, Variant and Dynamic are read as DuckDB JSON`, `# group: [scan]`, then the header plus `require json` after `require icu`):
```
query ITTTT
SELECT id, typeof(j), json_extract_string(j, '$.b.c'), json_extract_string(v, '$'), json_extract_string(dyn, '$') FROM ch.test_db.semi ORDER BY id;
----
1	JSON	x	str	42
2	JSON	NULL	7	hello
```

`test/sql/scan/times.test` (`# name: test/sql/scan/times.test`, `# description: Time and Time64`, `# group: [scan]`, then the header):
```
query IIII
SELECT * FROM ch.test_db.times ORDER BY id;
----
1	12:34:56	12:34:56.789	12:34:56.123456789
2	00:00:00	00:00:00	00:00:00
```

`test/sql/scan/errors.test` (`# name: test/sql/scan/errors.test`, `# description: values DuckDB cannot represent produce clear errors`, `# group: [scan]`, then the header):
```
statement error
SELECT * FROM ch.test_db.binary;
----
contains a value that is not valid UTF-8

query I
SELECT id FROM ch.test_db.binary;
----
1

statement error
SELECT total FROM ch.test_db.aggregates;
----
cannot be read by DuckDB

query II
SELECT k, last FROM ch.test_db.aggregates;
----
1	z

statement error
SELECT * FROM ch.test_db.bad_times;
----
outside the DuckDB TIME range

# the connection pool is still usable after errors
query I
SELECT count(*) FROM ch.test_db.t1;
----
3
```

`test/sql/scan/read_only.test` (`# name: test/sql/scan/read_only.test`, `# description: DML against an attached ClickHouse database is rejected`, `# group: [scan]`, then the header):
```
statement error
INSERT INTO ch.test_db.t1 VALUES (4, 'Dan', 1.0, TIMESTAMPTZ '2024-01-04 00:00:00+00');
----
clickhouse_scanner is read-only

statement error
UPDATE ch.test_db.t1 SET name = 'x';
----
clickhouse_scanner is read-only

statement error
DELETE FROM ch.test_db.t1;
----
clickhouse_scanner is read-only

query I
SELECT count(*) FROM ch.test_db.t1;
----
3
```
Note on expectations: they are written as DuckDB renders values in sqllogictest. If a value differs only in *formatting* (for example `0` vs `0.0`), fix the expectation. If the value itself is different (wrong UUID bytes, shifted timestamps, wrong decimal scale), fix the conversion.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make release && make smoke ARGS='test/sql/scan/*'`
Expected: FAIL with `Scanning ClickHouse tables is not implemented yet`.

- [ ] **Step 3: Conversion**

`src/include/clickhouse_conversion.hpp`:
```cpp
#pragma once

#include "duckdb/common/types/data_chunk.hpp"

#include <clickhouse/block.h>

namespace duckdb {

class ClickhouseConversion {
public:
	//! Converts rows [offset, offset + count) of every column of the block into the output chunk.
	//! column_names are only used for error messages.
	static void ConvertBlock(const clickhouse::Block &block, DataChunk &output, idx_t offset, idx_t count,
	                         const vector<string> &column_names);
	//! Converts rows [offset, offset + count) of a column into a flat vector, starting at row 0
	static void ConvertColumn(const clickhouse::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
	                          const string &column_name);
};

} // namespace duckdb
```

`src/clickhouse_conversion.cpp`:
```cpp
#include "clickhouse_conversion.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/unordered_map.hpp"

#include <clickhouse/client.h>
#include <clickhouse/columns/bool.h>

namespace duckdb {

namespace ch = clickhouse;

[[noreturn]] static void ThrowUnexpectedColumn(const ch::ColumnRef &column, const Vector &result,
                                               const string &column_name) {
	throw InternalException("ClickHouse column \"%s\" of type %s cannot be converted to DuckDB %s", column_name,
	                        column->Type()->GetName(), result.GetType().ToString());
}

static int64_t PowerOfTen(idx_t exponent) {
	int64_t result = 1;
	for (idx_t i = 0; i < exponent; i++) {
		result *= 10;
	}
	return result;
}

//! Rescales ticks between decimal precisions, flooring when precision is lost
static int64_t ScaleTicks(int64_t ticks, idx_t from_precision, idx_t to_precision) {
	if (from_precision == to_precision) {
		return ticks;
	}
	if (from_precision < to_precision) {
		return ticks * PowerOfTen(to_precision - from_precision);
	}
	auto divisor = PowerOfTen(from_precision - to_precision);
	auto result = ticks / divisor;
	if (ticks % divisor != 0 && ticks < 0) {
		result -= 1;
	}
	return result;
}

template <class T>
static void ConvertNumeric(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                           const string &column_name) {
	auto typed = column->As<ch::ColumnVector<T>>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto &data = typed->GetWritableData();
	memcpy(FlatVector::GetData<T>(result), data.data() + offset, count * sizeof(T));
}

static void ConvertBool(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                        const string &column_name) {
	auto data = FlatVector::GetData<bool>(result);
	if (auto typed = column->As<ch::ColumnBool>()) {
		for (idx_t i = 0; i < count; i++) {
			data[i] = typed->At(offset + i);
		}
		return;
	}
	if (auto typed = column->As<ch::ColumnUInt8>()) {
		for (idx_t i = 0; i < count; i++) {
			data[i] = typed->At(offset + i) != 0;
		}
		return;
	}
	ThrowUnexpectedColumn(column, result, column_name);
}

static hugeint_t ToHugeint(const ch::Int128 &value) {
	return hugeint_t(absl::Int128High64(value), absl::Int128Low64(value));
}

static void ConvertInt128(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                          const string &column_name) {
	auto typed = column->As<ch::ColumnInt128>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto data = FlatVector::GetData<hugeint_t>(result);
	for (idx_t i = 0; i < count; i++) {
		data[i] = ToHugeint(typed->At(offset + i));
	}
}

static void ConvertUInt128(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                           const string &column_name) {
	auto typed = column->As<ch::ColumnUInt128>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto data = FlatVector::GetData<uhugeint_t>(result);
	for (idx_t i = 0; i < count; i++) {
		auto value = typed->At(offset + i);
		data[i] = uhugeint_t(absl::Uint128High64(value), absl::Uint128Low64(value));
	}
}

template <class T>
static void ConvertDecimalTo(const std::shared_ptr<ch::ColumnDecimal> &typed, Vector &result, idx_t offset,
                             idx_t count) {
	auto data = FlatVector::GetData<T>(result);
	for (idx_t i = 0; i < count; i++) {
		data[i] = static_cast<T>(typed->At(offset + i));
	}
}

static void ConvertDecimal(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                           const string &column_name) {
	auto typed = column->As<ch::ColumnDecimal>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	switch (result.GetType().InternalType()) {
	case PhysicalType::INT16:
		ConvertDecimalTo<int16_t>(typed, result, offset, count);
		break;
	case PhysicalType::INT32:
		ConvertDecimalTo<int32_t>(typed, result, offset, count);
		break;
	case PhysicalType::INT64:
		ConvertDecimalTo<int64_t>(typed, result, offset, count);
		break;
	case PhysicalType::INT128: {
		auto data = FlatVector::GetData<hugeint_t>(result);
		for (idx_t i = 0; i < count; i++) {
			data[i] = ToHugeint(typed->At(offset + i));
		}
		break;
	}
	default:
		ThrowUnexpectedColumn(column, result, column_name);
	}
}

template <class COLUMN>
static void ConvertStringColumn(const std::shared_ptr<COLUMN> &typed, Vector &result, idx_t offset, idx_t count,
                                const string &column_name) {
	auto data = FlatVector::GetData<string_t>(result);
	auto &validity = FlatVector::Validity(result);
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			continue;
		}
		auto value = typed->At(offset + i);
		if (!ClickhouseUtils::IsValidUtf8(value.data(), value.size())) {
			throw InvalidInputException("ClickHouse column \"%s\" contains a value that is not valid UTF-8. Use "
			                            "clickhouse_query() with hex() or base64Encode() to read it",
			                            column_name);
		}
		data[i] = StringVector::AddString(result, value.data(), value.size());
	}
}

static void ConvertString(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                          const string &column_name) {
	if (auto typed = column->As<ch::ColumnString>()) {
		ConvertStringColumn(typed, result, offset, count, column_name);
		return;
	}
	if (auto typed = column->As<ch::ColumnFixedString>()) {
		ConvertStringColumn(typed, result, offset, count, column_name);
		return;
	}
	ThrowUnexpectedColumn(column, result, column_name);
}

static void ConvertDate(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                        const string &column_name) {
	auto data = FlatVector::GetData<date_t>(result);
	if (auto typed = column->As<ch::ColumnDate>()) {
		for (idx_t i = 0; i < count; i++) {
			data[i] = date_t(static_cast<int32_t>(typed->RawAt(offset + i)));
		}
		return;
	}
	if (auto typed = column->As<ch::ColumnDate32>()) {
		for (idx_t i = 0; i < count; i++) {
			data[i] = date_t(typed->RawAt(offset + i));
		}
		return;
	}
	ThrowUnexpectedColumn(column, result, column_name);
}

static void ConvertTimestamp(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                             const string &column_name) {
	auto data = FlatVector::GetData<timestamp_tz_t>(result);
	if (auto typed = column->As<ch::ColumnDateTime>()) {
		for (idx_t i = 0; i < count; i++) {
			auto seconds = static_cast<int64_t>(typed->RawAt(offset + i));
			data[i] = timestamp_tz_t(seconds * Interval::MICROS_PER_SEC);
		}
		return;
	}
	if (auto typed = column->As<ch::ColumnDateTime64>()) {
		auto precision = typed->GetPrecision();
		for (idx_t i = 0; i < count; i++) {
			data[i] = timestamp_tz_t(ScaleTicks(typed->At(offset + i), precision, 6));
		}
		return;
	}
	ThrowUnexpectedColumn(column, result, column_name);
}

static void ConvertTime(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                        const string &column_name) {
	auto time32 = column->As<ch::ColumnTime>();
	auto time64 = column->As<ch::ColumnTime64>();
	if (!time32 && !time64) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	idx_t precision = time32 ? 0 : time64->GetPrecision();
	auto nanoseconds = result.GetType().id() == LogicalTypeId::TIME_NS;
	auto max_ticks = Interval::SECS_PER_DAY * PowerOfTen(precision);
	auto &validity = FlatVector::Validity(result);
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			continue;
		}
		int64_t ticks = time32 ? static_cast<int64_t>(time32->At(offset + i)) : time64->At(offset + i);
		if (ticks < 0 || ticks > max_ticks) {
			throw ConversionException(
			    "ClickHouse column \"%s\" contains a time outside the DuckDB TIME range (00:00:00 - 24:00:00)",
			    column_name);
		}
		if (nanoseconds) {
			FlatVector::GetData<dtime_ns_t>(result)[i] = dtime_ns_t(ScaleTicks(ticks, precision, 9));
		} else {
			FlatVector::GetData<dtime_t>(result)[i] = dtime_t(ScaleTicks(ticks, precision, 6));
		}
	}
}

static void ConvertUUID(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                        const string &column_name) {
	auto typed = column->As<ch::ColumnUUID>();
	if (!typed) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto data = FlatVector::GetData<hugeint_t>(result);
	for (idx_t i = 0; i < count; i++) {
		auto value = typed->At(offset + i);
		data[i] = UUID::FromUHugeint(uhugeint_t(value.first, value.second));
	}
}

template <class T>
static void WriteEnumPositions(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                               const unordered_map<int16_t, uint32_t> &positions, const string &column_name) {
	auto data = FlatVector::GetData<T>(result);
	auto &validity = FlatVector::Validity(result);
	auto enum8 = column->As<ch::ColumnEnum8>();
	auto enum16 = column->As<ch::ColumnEnum16>();
	for (idx_t i = 0; i < count; i++) {
		if (!validity.RowIsValid(i)) {
			data[i] = 0;
			continue;
		}
		int16_t value = enum8 ? static_cast<int16_t>(enum8->At(offset + i)) : enum16->At(offset + i);
		auto position = positions.find(value);
		if (position == positions.end()) {
			throw ConversionException("ClickHouse column \"%s\" contains unknown enum value %d", column_name,
			                          static_cast<int32_t>(value));
		}
		data[i] = static_cast<T>(position->second);
	}
}

static void ConvertEnum(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                        const string &column_name) {
	if (!column->As<ch::ColumnEnum8>() && !column->As<ch::ColumnEnum16>()) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	// map ClickHouse enum values to positions in the DuckDB ENUM dictionary (matched by label)
	auto enum_type = column->Type()->As<ch::EnumType>();
	unordered_map<int16_t, uint32_t> positions;
	for (auto it = enum_type->BeginValueToName(); it != enum_type->EndValueToName(); ++it) {
		auto position = EnumType::GetPos(result.GetType(), string_t(it->second.data(), it->second.size()));
		if (position < 0) {
			ThrowUnexpectedColumn(column, result, column_name);
		}
		positions[it->first] = static_cast<uint32_t>(position);
	}
	switch (result.GetType().InternalType()) {
	case PhysicalType::UINT8:
		WriteEnumPositions<uint8_t>(column, result, offset, count, positions, column_name);
		break;
	case PhysicalType::UINT16:
		WriteEnumPositions<uint16_t>(column, result, offset, count, positions, column_name);
		break;
	case PhysicalType::UINT32:
		WriteEnumPositions<uint32_t>(column, result, offset, count, positions, column_name);
		break;
	default:
		ThrowUnexpectedColumn(column, result, column_name);
	}
}

//! Array(T) -> LIST(T); Array(Tuple(K, V)) -> MAP(K, V), which has the same physical layout
static void ConvertList(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                        const string &column_name) {
	auto array = column->As<ch::ColumnArray>();
	if (!array) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	auto entries = FlatVector::GetData<list_entry_t>(result);
	idx_t child_start = count == 0 ? 0 : array->GetOffset(offset);
	idx_t total = 0;
	for (idx_t i = 0; i < count; i++) {
		auto size = array->GetSize(offset + i);
		entries[i].offset = total;
		entries[i].length = size;
		total += size;
	}
	ListVector::Reserve(result, total);
	ListVector::SetListSize(result, total);
	if (total > 0) {
		ClickhouseConversion::ConvertColumn(array->GetData(), ListVector::GetEntry(result), child_start, total,
		                                    column_name);
	}
}

static void ConvertStruct(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                          const string &column_name) {
	auto tuple = column->As<ch::ColumnTuple>();
	auto &children = StructVector::GetEntries(result);
	if (!tuple || tuple->TupleSize() != children.size()) {
		ThrowUnexpectedColumn(column, result, column_name);
	}
	for (idx_t c = 0; c < children.size(); c++) {
		ClickhouseConversion::ConvertColumn(tuple->At(c), *children[c], offset, count, column_name);
	}
}

void ClickhouseConversion::ConvertColumn(const ch::ColumnRef &column, Vector &result, idx_t offset, idx_t count,
                                         const string &column_name) {
	D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
	auto values = column;
	auto &validity = FlatVector::Validity(result);
	if (auto nullable = column->As<ch::ColumnNullable>()) {
		for (idx_t i = 0; i < count; i++) {
			validity.Set(i, !nullable->IsNull(offset + i));
		}
		values = nullable->Nested();
	}
	if (values->As<ch::ColumnNothing>()) {
		for (idx_t i = 0; i < count; i++) {
			validity.SetInvalid(i);
		}
		return;
	}
	switch (result.GetType().id()) {
	case LogicalTypeId::BOOLEAN:
		ConvertBool(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::TINYINT:
		ConvertNumeric<int8_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::SMALLINT:
		ConvertNumeric<int16_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::INTEGER:
		ConvertNumeric<int32_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::BIGINT:
		ConvertNumeric<int64_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UTINYINT:
		ConvertNumeric<uint8_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::USMALLINT:
		ConvertNumeric<uint16_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UINTEGER:
		ConvertNumeric<uint32_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UBIGINT:
		ConvertNumeric<uint64_t>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::FLOAT:
		ConvertNumeric<float>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::DOUBLE:
		ConvertNumeric<double>(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::HUGEINT:
		ConvertInt128(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UHUGEINT:
		ConvertUInt128(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::DECIMAL:
		ConvertDecimal(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::VARCHAR:
		ConvertString(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::DATE:
		ConvertDate(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
		ConvertTimestamp(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
		ConvertTime(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::UUID:
		ConvertUUID(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::ENUM:
		ConvertEnum(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		ConvertList(values, result, offset, count, column_name);
		break;
	case LogicalTypeId::STRUCT:
		ConvertStruct(values, result, offset, count, column_name);
		break;
	default:
		ThrowUnexpectedColumn(values, result, column_name);
	}
}

void ClickhouseConversion::ConvertBlock(const ch::Block &block, DataChunk &output, idx_t offset, idx_t count,
                                        const vector<string> &column_names) {
	if (output.ColumnCount() == 0) {
		// no columns requested (e.g. count(*)): only the row count matters
		output.SetCardinality(count);
		return;
	}
	if (block.GetColumnCount() != output.ColumnCount()) {
		throw InternalException("ClickHouse returned %llu columns, expected %llu",
		                        static_cast<uint64_t>(block.GetColumnCount()),
		                        static_cast<uint64_t>(output.ColumnCount()));
	}
	for (idx_t c = 0; c < output.ColumnCount(); c++) {
		ConvertColumn(block[c], output.data[c], offset, count, column_names[c]);
	}
	output.SetCardinality(count);
}

} // namespace duckdb
```
In `ConvertEnum`, `EnumType` without a namespace prefix means `duckdb::EnumType`, and `ch::EnumType` is clickhouse-cpp's. `column->Type()` returns a `std::shared_ptr<clickhouse::Type>`. Its `As<T>()` is a `static_cast` that returns a raw pointer.

- [ ] **Step 4: The scan**

`src/include/clickhouse_scanner.hpp`:
```cpp
#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/function/table_function.hpp"
#include "storage/clickhouse_connection_pool.hpp"

namespace duckdb {

struct ClickhouseScanBindData : public TableFunctionData {
	//! Pool of the attached catalog, or a private pool for clickhouse_scan() without ATTACH
	shared_ptr<ClickhouseConnectionPool> pool;
	//! Scanned table (attached tables and clickhouse_scan)
	string database;
	string table;
	//! Wrapped query (clickhouse_query); when set, database/table are unused
	string query;
	vector<ClickhouseColumnInfo> columns;
	optional_idx approx_rows;
	//! ch_filter_pushdown at bind time
	bool filter_pushdown = true;
	//! Filled by ClickhouseOptimizer, e.g. " ORDER BY `n` DESC NULLS LAST" and " LIMIT 3"
	string order_by_clause;
	string limit_clause;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

class ClickhouseScanFunction : public TableFunction {
public:
	ClickhouseScanFunction();

	//! Installs the scan callbacks shared by clickhouse_scan and clickhouse_query
	static void SetScanCallbacks(TableFunction &function);
	//! The ClickHouse query for the projected columns, pushed-down filters and ORDER BY / LIMIT
	static string BuildQuery(const ClickhouseScanBindData &bind_data, const vector<column_t> &column_ids,
	                         optional_ptr<TableFilterSet> filters);
	static void SetReturnTypes(const ClickhouseScanBindData &bind_data, vector<LogicalType> &return_types,
	                           vector<string> &names);
	static bool IsClickhouseScan(const string &function_name);
};

} // namespace duckdb
```

`src/clickhouse_scanner.cpp`:
```cpp
#include "clickhouse_scanner.hpp"

#include "clickhouse_conversion.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

#include <optional>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Bind data
//===--------------------------------------------------------------------===//
unique_ptr<FunctionData> ClickhouseScanBindData::Copy() const {
	auto result = make_uniq<ClickhouseScanBindData>();
	result->pool = pool;
	result->database = database;
	result->table = table;
	result->query = query;
	result->columns = columns;
	result->approx_rows = approx_rows;
	result->filter_pushdown = filter_pushdown;
	result->order_by_clause = order_by_clause;
	result->limit_clause = limit_clause;
	return std::move(result);
}

bool ClickhouseScanBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<ClickhouseScanBindData>();
	return pool == other.pool && database == other.database && table == other.table && query == other.query &&
	       filter_pushdown == other.filter_pushdown && order_by_clause == other.order_by_clause &&
	       limit_clause == other.limit_clause;
}

//===--------------------------------------------------------------------===//
// State
//===--------------------------------------------------------------------===//
//! One ClickHouse query per scan. Workers take turns pulling blocks from it (under `lock`) and convert them in
//! parallel.
struct ClickhouseScanGlobalState : public GlobalTableFunctionState {
	~ClickhouseScanGlobalState() override {
		// the scan stopped early (LIMIT reached, error, interrupt): stop the server-side query
		if (connection && connection->IsQueryRunning()) {
			connection->Cancel();
		}
	}

	idx_t MaxThreads() const override {
		return max_threads;
	}

	mutex lock;
	ClickhousePoolConnection connection;
	string sql;
	vector<string> column_names;
	bool finished = false;
	idx_t next_batch_index = 0;
	idx_t max_threads = 1;
};

struct ClickhouseScanLocalState : public LocalTableFunctionState {
	std::optional<clickhouse::Block> block;
	idx_t offset = 0;
	idx_t batch_index = 0;
};

//===--------------------------------------------------------------------===//
// Query generation
//===--------------------------------------------------------------------===//
string ClickhouseScanFunction::BuildQuery(const ClickhouseScanBindData &bind_data, const vector<column_t> &column_ids,
                                          optional_ptr<TableFilterSet> filters) {
	vector<string> select_list;
	for (auto column_id : column_ids) {
		if (IsVirtualColumn(column_id)) {
			// row id / empty projection (e.g. count(*)): any value works, only the row count matters
			select_list.push_back("NULL");
			continue;
		}
		auto &column = bind_data.columns[column_id];
		if (!column.readable) {
			throw NotImplementedException("Column \"%s\" has ClickHouse type %s, which cannot be read by DuckDB. Use "
			                              "clickhouse_query() with finalizeAggregation(%s) instead",
			                              column.name, column.clickhouse_type, column.name);
		}
		select_list.push_back(
		    ClickhouseTypes::ReadExpression(column.type_node, ClickhouseUtils::QuoteIdentifier(column.name)));
	}
	if (select_list.empty()) {
		select_list.push_back("NULL");
	}
	string source;
	if (bind_data.query.empty()) {
		source = ClickhouseUtils::QuoteIdentifier(bind_data.database) + "." +
		         ClickhouseUtils::QuoteIdentifier(bind_data.table);
	} else {
		source = "(" + bind_data.query + ")";
	}
	auto sql = "SELECT " + StringUtil::Join(select_list, ", ") + " FROM " + source;
	sql += bind_data.order_by_clause;
	sql += bind_data.limit_clause;
	return sql;
}

void ClickhouseScanFunction::SetReturnTypes(const ClickhouseScanBindData &bind_data, vector<LogicalType> &return_types,
                                            vector<string> &names) {
	for (auto &column : bind_data.columns) {
		names.push_back(column.name);
		return_types.push_back(column.type);
	}
}

bool ClickhouseScanFunction::IsClickhouseScan(const string &function_name) {
	return function_name == "clickhouse_scan" || function_name == "clickhouse_query";
}

//===--------------------------------------------------------------------===//
// Callbacks
//===--------------------------------------------------------------------===//
static unique_ptr<GlobalTableFunctionState> ClickhouseInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<ClickhouseScanBindData>();
	auto result = make_uniq<ClickhouseScanGlobalState>();
	result->sql = ClickhouseScanFunction::BuildQuery(bind_data, input.column_ids, input.filters);
	for (auto column_id : input.column_ids) {
		result->column_names.push_back(IsVirtualColumn(column_id) ? "rowid" : bind_data.columns[column_id].name);
	}
	// a pushed-down ORDER BY must reach DuckDB in order: read with a single thread
	result->max_threads =
	    bind_data.order_by_clause.empty() ? MaxValue<idx_t>(1, context.db->NumberOfThreads()) : idx_t(1);
	result->connection = bind_data.pool->GetConnection();
	result->connection->BeginQuery(result->sql);
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> ClickhouseInitLocal(ExecutionContext &context,
                                                               TableFunctionInitInput &input,
                                                               GlobalTableFunctionState *global_state) {
	return make_uniq<ClickhouseScanLocalState>();
}

//! Takes the next non-empty block of the query for this worker; false when the query is exhausted
static bool FetchNextBlock(ClickhouseScanGlobalState &gstate, ClickhouseScanLocalState &lstate) {
	lock_guard<mutex> guard(gstate.lock);
	lstate.block.reset();
	lstate.offset = 0;
	while (!gstate.finished) {
		auto block = gstate.connection->NextBlock();
		if (!block) {
			gstate.finished = true;
			// hand the connection back to the pool as soon as the query is done
			gstate.connection = ClickhousePoolConnection();
			break;
		}
		if (block->GetRowCount() == 0) {
			continue;
		}
		lstate.block = std::move(block);
		lstate.batch_index = gstate.next_batch_index++;
		return true;
	}
	return false;
}

static void ClickhouseScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &gstate = data.global_state->Cast<ClickhouseScanGlobalState>();
	auto &lstate = data.local_state->Cast<ClickhouseScanLocalState>();
	while (true) {
		if (lstate.block && lstate.offset < lstate.block->GetRowCount()) {
			auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, lstate.block->GetRowCount() - lstate.offset);
			ClickhouseConversion::ConvertBlock(*lstate.block, output, lstate.offset, count, gstate.column_names);
			lstate.offset += count;
			return;
		}
		if (!FetchNextBlock(gstate, lstate)) {
			output.SetCardinality(0);
			return;
		}
	}
}

static OperatorPartitionData ClickhouseGetPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("ClickhouseScan::GetPartitionData: partition columns are not supported");
	}
	auto &lstate = input.local_state->Cast<ClickhouseScanLocalState>();
	return OperatorPartitionData(lstate.batch_index);
}

static unique_ptr<NodeStatistics> ClickhouseScanCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<ClickhouseScanBindData>();
	if (!bind_data.approx_rows.IsValid()) {
		return nullptr;
	}
	return make_uniq<NodeStatistics>(bind_data.approx_rows.GetIndex());
}

static InsertionOrderPreservingMap<string> ClickhouseScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<ClickhouseScanBindData>();
	if (bind_data.query.empty()) {
		result["Table"] = bind_data.database + "." + bind_data.table;
	} else {
		result["Query"] = bind_data.query;
	}
	auto pushed = bind_data.order_by_clause + bind_data.limit_clause;
	StringUtil::Trim(pushed);
	if (!pushed.empty()) {
		result["Pushed Down"] = pushed;
	}
	return result;
}

static InsertionOrderPreservingMap<string> ClickhouseScanDynamicToString(TableFunctionDynamicToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	if (input.global_state) {
		result["ClickHouse Query"] = input.global_state->Cast<ClickhouseScanGlobalState>().sql;
	}
	return result;
}

void ClickhouseScanFunction::SetScanCallbacks(TableFunction &function) {
	function.init_global = ClickhouseInitGlobal;
	function.init_local = ClickhouseInitLocal;
	function.function = ClickhouseScan;
	function.get_partition_data = ClickhouseGetPartitionData;
	function.cardinality = ClickhouseScanCardinality;
	function.to_string = ClickhouseScanToString;
	function.dynamic_to_string = ClickhouseScanDynamicToString;
	function.projection_pushdown = true;
	function.filter_pushdown = false;
}

ClickhouseScanFunction::ClickhouseScanFunction()
    : TableFunction("clickhouse_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                    ClickhouseScan) {
	SetScanCallbacks(*this);
}

} // namespace duckdb
```

- [ ] **Step 5: Hook the scan into the table entry**

In `src/storage/clickhouse_table_entry.cpp` add the includes `#include "clickhouse_scanner.hpp"` and `#include "storage/clickhouse_catalog.hpp"`, and replace `GetScanFunction`:
```cpp
TableFunction ClickhouseTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto result = make_uniq<ClickhouseScanBindData>();
	result->pool = catalog.Cast<ClickhouseCatalog>().GetConnectionPoolPtr();
	result->database = schema.name;
	result->table = name;
	result->columns = clickhouse_columns;
	result->approx_rows = approx_rows;
	bind_data = std::move(result);
	return ClickhouseScanFunction();
}
```

Add `clickhouse_conversion.cpp` and `clickhouse_scanner.cpp` to `clickhouse_ext` in `src/CMakeLists.txt`.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `make release && make smoke`
Expected: `All tests passed`. Then run the debug build: `make debug && make smoke SMOKE_BUILD=debug ARGS='test/sql/scan/*'`. Debug builds run DuckDB's vector verification and catch malformed vectors (for example list sizes or validity).
If the server rejects reads of `Time`/`Time64` columns because they are experimental in 25.8, add `settings=enable_time_time64_type=1` to the ATTACH string in `times.test` and `errors.test`, rather than to the extension.
DuckDB's binder may reject `UPDATE`/`DELETE` with its own message before `PlanUpdate`/`PlanDelete` runs. If so, accept that message in `read_only.test`. The contract is only that no write reaches ClickHouse, and the final `count(*) = 3` checks that.

- [ ] **Step 7: Commit**

```bash
git add src test
git commit -m "feat: parallel, cancellable ClickHouse table scans with full type conversion

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---
### Task 7: Filter pushdown

**Files:**
- Create: `src/include/clickhouse_filter_pushdown.hpp`, `src/clickhouse_filter_pushdown.cpp`
- Modify: `src/include/clickhouse_scanner.hpp`, `src/clickhouse_scanner.cpp`, `src/storage/clickhouse_table_entry.cpp`, `src/clickhouse_scanner_extension.cpp`, `src/CMakeLists.txt`
- Test: `test/sql/pushdown/filters.test`

**Interfaces:**
- Consumes: `ClickhouseScanBindData`, `ClickhouseTypes::SupportsPushdown`, `ClickhouseUtils::{QuoteIdentifier, QuoteLiteral}`.
- Produces:
  - `ClickhouseFilterPushdown::TransformFilters(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters, const vector<ClickhouseColumnInfo> &columns) -> string` (the WHERE body, or empty).
  - `ClickhouseFilterPushdown::TransformConstant(const Value &) -> string`.
  - `ClickhouseScanFunction::FilterPushdownEnabled(ClientContext &) -> bool` (reads `ch_filter_pushdown`).
  - Setting `ch_filter_pushdown` (BOOLEAN, default true).

- [ ] **Step 1: Write the failing test**

`test/sql/pushdown/filters.test`:
```
# name: test/sql/pushdown/filters.test
# description: filters are evaluated by ClickHouse when the translation is exact
# group: [pushdown]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

require icu

statement ok
SET TimeZone = 'UTC';

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

# integers
query II
SELECT id, name FROM ch.test_db.t1 WHERE id = 2;
----
2	Bob

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id, name FROM ch.test_db.t1 WHERE id = 2;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT `id`, `name` FROM `test_db`.`t1` WHERE `id` = 2".*

query I
SELECT id FROM ch.test_db.t1 WHERE id = 1 OR id = 3 ORDER BY id;
----
1
3

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.t1 WHERE id = 1 OR id = 3;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT `id` FROM `test_db`.`t1` WHERE .*`id`.*1.*3.*".*

# strings
query I
SELECT id FROM ch.test_db.t1 WHERE name IN ('Alice', 'Charlie') ORDER BY id;
----
1
3

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.t1 WHERE name IN ('Alice', 'Charlie');
----
analyzed_plan	<REGEX>:.*WHERE .*`name`.*'Alice'.*'Charlie'.*

query I
SELECT count(*) FROM ch.test_db.t1 WHERE name = 'O''Brien\';
----
0

query I
SELECT id FROM ch.test_db.nullables WHERE lcs = 'y';
----
2

# ranges on one column
query I
SELECT id FROM ch.test_db.nullables WHERE i > 0 AND i < 100;
----
2

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.nullables WHERE i > 0 AND i < 100;
----
analyzed_plan	<REGEX>:.*WHERE .*`i` > 0.*`i` < 100.*

# NULL checks
query I
SELECT id FROM ch.test_db.nullables WHERE i IS NULL;
----
1

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.nullables WHERE i IS NULL;
----
analyzed_plan	<REGEX>:.*WHERE `i` IS NULL.*

# temporal types
query I
SELECT id FROM ch.test_db.t1 WHERE created_at >= TIMESTAMPTZ '2024-01-02 00:00:00+00' ORDER BY id;
----
2
3

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.t1 WHERE created_at >= TIMESTAMPTZ '2024-01-02 00:00:00+00';
----
analyzed_plan	<REGEX>:.*`created_at` >= fromUnixTimestamp64Micro\(1704153600000000, 'UTC'\).*

query I
SELECT id FROM ch.test_db.scalars WHERE d = DATE '2024-02-29';
----
1

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.scalars WHERE d = DATE '2024-02-29';
----
analyzed_plan	<REGEX>:.*`d` = toDate32\('2024-02-29'\).*

# decimals, enums and booleans
query I
SELECT id FROM ch.test_db.scalars WHERE d9 > 1000.00::DECIMAL(9, 2);
----
1

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.scalars WHERE d9 > 1000.00::DECIMAL(9, 2);
----
analyzed_plan	<REGEX>:.*`d9` > toDecimal128\('1000.00', 2\).*

query I
SELECT id FROM ch.test_db.scalars WHERE e8 = 'blue';
----
1

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.scalars WHERE e8 = 'blue';
----
analyzed_plan	<REGEX>:.*`e8` = 'blue'.*

query I
SELECT id FROM ch.test_db.scalars WHERE b;
----
1

# not pushed: floats (NaN semantics differ) and UUIDs (ordering differs); DuckDB filters instead
query I
SELECT id FROM ch.test_db.t1 WHERE value > 100 ORDER BY id;
----
2
3

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.t1 WHERE value > 100;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT [^"]*FROM `test_db`.`t1`".*

query I
SELECT id FROM ch.test_db.scalars WHERE uuid = '61f0c404-5cb3-11e7-907b-a6006ad3dba0';
----
1

# pushed and non-pushed filters together
query I
SELECT id FROM ch.test_db.t1 WHERE id >= 2 AND value < 200;
----
2

# the setting turns pushdown off
statement ok
SET ch_filter_pushdown = false;

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id, name FROM ch.test_db.t1 WHERE id = 2;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT `id`, `name` FROM `test_db`.`t1`".*

query II
SELECT id, name FROM ch.test_db.t1 WHERE id = 2;
----
2	Bob
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/pushdown/filters.test`
Expected: FAIL on the first EXPLAIN. The results are correct because DuckDB filters locally, but the generated query has no `WHERE`. The `SET ch_filter_pushdown` statement also fails because the setting does not exist yet.

- [ ] **Step 3: Implement the translation**

`src/include/clickhouse_filter_pushdown.hpp`:
```cpp
#pragma once

#include "clickhouse_types.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

class ClickhouseFilterPushdown {
public:
	//! WHERE clause body for the filters DuckDB pushed into the scan (empty when nothing restricts the rows).
	//! filters is keyed by position in column_ids; column_ids index into columns.
	static string TransformFilters(const vector<column_t> &column_ids, optional_ptr<TableFilterSet> filters,
	                               const vector<ClickhouseColumnInfo> &columns);
	//! Translates one filter on `column`. Returns "" when the filter does not restrict rows (dynamic/bloom filters,
	//! or optional filters that cannot be translated). Throws for required filters that cannot be translated.
	static string TransformFilter(const string &column, const TableFilter &filter, bool optional = false);
	static string TransformConstant(const Value &value);
};

} // namespace duckdb
```

`src/clickhouse_filter_pushdown.cpp`:
```cpp
#include "clickhouse_filter_pushdown.hpp"

#include "clickhouse_utils.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/planner/filter/conjunction_filter.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"

namespace duckdb {

static string TransformComparison(ExpressionType type) {
	switch (type) {
	case ExpressionType::COMPARE_EQUAL:
		return "=";
	case ExpressionType::COMPARE_NOTEQUAL:
		return "!=";
	case ExpressionType::COMPARE_LESSTHAN:
		return "<";
	case ExpressionType::COMPARE_GREATERTHAN:
		return ">";
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return "<=";
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ">=";
	default:
		throw NotImplementedException("ClickHouse filter pushdown: unsupported comparison %s",
		                              EnumUtil::ToString(type));
	}
}

string ClickhouseFilterPushdown::TransformConstant(const Value &value) {
	switch (value.type().id()) {
	case LogicalTypeId::BOOLEAN:
		return BooleanValue::Get(value) ? "true" : "false";
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return value.ToString();
	case LogicalTypeId::DECIMAL:
		// a plain 12.5 literal would be a Float64 in ClickHouse, which does not compare with Decimal columns
		return StringUtil::Format("toDecimal128(%s, %d)", ClickhouseUtils::QuoteLiteral(value.ToString()),
		                          static_cast<int32_t>(DecimalType::GetScale(value.type())));
	case LogicalTypeId::VARCHAR:
		return ClickhouseUtils::QuoteLiteral(StringValue::Get(value));
	case LogicalTypeId::ENUM:
		return ClickhouseUtils::QuoteLiteral(value.ToString());
	case LogicalTypeId::DATE: {
		auto date = DateValue::Get(value);
		if (!Date::IsFinite(date)) {
			throw NotImplementedException("ClickHouse filter pushdown: infinite dates are not supported");
		}
		return "toDate32(" + ClickhouseUtils::QuoteLiteral(Date::ToString(date)) + ")";
	}
	case LogicalTypeId::TIMESTAMP_TZ: {
		auto timestamp = TimestampTZValue::Get(value);
		if (!Timestamp::IsFinite(timestamp)) {
			throw NotImplementedException("ClickHouse filter pushdown: infinite timestamps are not supported");
		}
		return StringUtil::Format("fromUnixTimestamp64Micro(%d, 'UTC')", timestamp.value);
	}
	default:
		throw NotImplementedException("ClickHouse filter pushdown: unsupported constant type %s",
		                              value.type().ToString());
	}
}

string ClickhouseFilterPushdown::TransformFilter(const string &column, const TableFilter &filter, bool optional) {
	switch (filter.filter_type) {
	case TableFilterType::IS_NULL:
		return column + " IS NULL";
	case TableFilterType::IS_NOT_NULL:
		return column + " IS NOT NULL";
	case TableFilterType::CONSTANT_COMPARISON: {
		auto &constant_filter = filter.Cast<ConstantFilter>();
		return column + " " + TransformComparison(constant_filter.comparison_type) + " " +
		       TransformConstant(constant_filter.constant);
	}
	case TableFilterType::IN_FILTER: {
		auto &in_filter = filter.Cast<InFilter>();
		vector<string> values;
		for (auto &value : in_filter.values) {
			values.push_back(TransformConstant(value));
		}
		return column + " IN (" + StringUtil::Join(values, ", ") + ")";
	}
	case TableFilterType::CONJUNCTION_AND: {
		auto &conjunction = filter.Cast<ConjunctionAndFilter>();
		vector<string> parts;
		for (auto &child : conjunction.child_filters) {
			auto part = TransformFilter(column, *child, optional);
			if (!part.empty()) {
				parts.push_back(part);
			}
		}
		if (parts.empty()) {
			return string();
		}
		return "(" + StringUtil::Join(parts, " AND ") + ")";
	}
	case TableFilterType::CONJUNCTION_OR: {
		auto &conjunction = filter.Cast<ConjunctionOrFilter>();
		vector<string> parts;
		for (auto &child : conjunction.child_filters) {
			auto part = TransformFilter(column, *child, optional);
			if (part.empty()) {
				// one branch does not restrict rows, so neither does the OR
				return string();
			}
			parts.push_back(part);
		}
		return "(" + StringUtil::Join(parts, " OR ") + ")";
	}
	case TableFilterType::OPTIONAL_FILTER: {
		auto &optional_filter = filter.Cast<OptionalFilter>();
		if (!optional_filter.child_filter) {
			return string();
		}
		try {
			return TransformFilter(column, *optional_filter.child_filter, true);
		} catch (NotImplementedException &) {
			return string();
		}
	}
	case TableFilterType::DYNAMIC_FILTER:
	case TableFilterType::BLOOM_FILTER:
		// runtime hints from joins / top-n: the operators above the scan still enforce them
		return string();
	default:
		if (optional) {
			return string();
		}
		throw NotImplementedException("ClickHouse filter pushdown: unsupported filter type %s",
		                              EnumUtil::ToString(filter.filter_type));
	}
}

string ClickhouseFilterPushdown::TransformFilters(const vector<column_t> &column_ids,
                                                  optional_ptr<TableFilterSet> filters,
                                                  const vector<ClickhouseColumnInfo> &columns) {
	if (!filters || filters->filters.empty()) {
		return string();
	}
	vector<string> conditions;
	for (auto &entry : filters->filters) {
		auto column_id = column_ids[entry.first];
		if (IsVirtualColumn(column_id)) {
			throw InternalException("ClickHouse filter pushdown: unexpected filter on a virtual column");
		}
		auto column = ClickhouseUtils::QuoteIdentifier(columns[column_id].name);
		auto condition = TransformFilter(column, *entry.second);
		if (!condition.empty()) {
			conditions.push_back(condition);
		}
	}
	return StringUtil::Join(conditions, " AND ");
}

} // namespace duckdb
```

- [ ] **Step 4: Enable pushdown in the scan**

In `src/include/clickhouse_scanner.hpp` add to `ClickhouseScanFunction`:
```cpp
	//! ch_filter_pushdown
	static bool FilterPushdownEnabled(ClientContext &context);
```

In `src/clickhouse_scanner.cpp` add the include `#include "clickhouse_filter_pushdown.hpp"`. In `BuildQuery`, insert these lines between building `sql` and appending `order_by_clause`:
```cpp
	auto where_clause = ClickhouseFilterPushdown::TransformFilters(column_ids, filters, bind_data.columns);
	if (!where_clause.empty()) {
		sql += " WHERE " + where_clause;
	}
```
Add these functions above `SetScanCallbacks`:
```cpp
//! DuckDB only hands us filters on columns for which this returns true; it evaluates all others itself
static bool ClickhouseSupportsPushdownType(const FunctionData &bind_data_p, idx_t column_index) {
	auto &bind_data = bind_data_p.Cast<ClickhouseScanBindData>();
	if (!bind_data.filter_pushdown || column_index >= bind_data.columns.size()) {
		return false;
	}
	return ClickhouseTypes::SupportsPushdown(bind_data.columns[column_index].type_node);
}

bool ClickhouseScanFunction::FilterPushdownEnabled(ClientContext &context) {
	Value value;
	if (context.TryGetCurrentSetting("ch_filter_pushdown", value) && !value.IsNull()) {
		return BooleanValue::Get(value);
	}
	return true;
}
```
In `SetScanCallbacks`, replace `function.filter_pushdown = false;` with:
```cpp
	function.filter_pushdown = true;
	function.supports_pushdown_type = ClickhouseSupportsPushdownType;
```

In `src/storage/clickhouse_table_entry.cpp` `GetScanFunction`, before `bind_data = std::move(result);` add:
```cpp
	result->filter_pushdown = ClickhouseScanFunction::FilterPushdownEnabled(context);
```

Register the setting in `LoadInternal` with the other `AddExtensionOption` calls:
```cpp
	config.AddExtensionOption("ch_filter_pushdown", "Push filters down into the queries sent to ClickHouse",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
```

Add `clickhouse_filter_pushdown.cpp` to `clickhouse_ext` in `src/CMakeLists.txt`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make release && make smoke`
Expected: `All tests passed`.
If an EXPLAIN regex fails but the result queries pass, print the plan with `./build/release/duckdb -unsigned` and compare. DuckDB may express the same filter differently, for example `IN` instead of `OR`. Loosen the regex only when the generated SQL is still correct.

- [ ] **Step 6: Commit**

```bash
git add src test
git commit -m "feat: push exact filters down into ClickHouse queries

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 8: LIMIT and ORDER BY … LIMIT pushdown

**Files:**
- Create: `src/include/storage/clickhouse_optimizer.hpp`, `src/storage/clickhouse_optimizer.cpp`
- Modify: `src/clickhouse_scanner_extension.cpp`, `src/storage/CMakeLists.txt`
- Test: `test/sql/pushdown/limit.test`

**Interfaces:**
- Consumes: `ClickhouseScanBindData::{order_by_clause, limit_clause, filter_pushdown, columns}`, `ClickhouseScanFunction::IsClickhouseScan`, `ClickhouseTypes::SupportsPushdown`.
- Produces: `ClickhouseOptimizer::Optimize(OptimizerExtensionInput &, unique_ptr<LogicalOperator> &)` and the setting `ch_order_pushdown` (BOOLEAN, default true).

- [ ] **Step 1: Write the failing test**

`test/sql/pushdown/limit.test`:
```
# name: test/sql/pushdown/limit.test
# description: LIMIT and ORDER BY ... LIMIT run inside ClickHouse when that is exact
# group: [pushdown]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

query I
SELECT n FROM ch.test_db.big ORDER BY n DESC LIMIT 3;
----
9999999
9999998
9999997

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT n FROM ch.test_db.big ORDER BY n DESC LIMIT 3;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT `n` FROM `test_db`.`big` ORDER BY `n` DESC NULLS LAST LIMIT 3".*

query II
SELECT n, s FROM ch.test_db.big ORDER BY n LIMIT 2 OFFSET 5;
----
5	5
6	6

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT n, s FROM ch.test_db.big ORDER BY n LIMIT 2 OFFSET 5;
----
analyzed_plan	<REGEX>:.*ORDER BY `n` ASC NULLS LAST LIMIT 2 OFFSET 5".*

# through a projection with an alias
query I
SELECT n AS x FROM ch.test_db.big ORDER BY x DESC LIMIT 1;
----
9999999

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT n AS x FROM ch.test_db.big ORDER BY x DESC LIMIT 1;
----
analyzed_plan	<REGEX>:.*ORDER BY `n` DESC NULLS LAST LIMIT 1".*

# together with a pushed filter
query I
SELECT n FROM ch.test_db.big WHERE n < 100 ORDER BY n DESC LIMIT 1;
----
99

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT n FROM ch.test_db.big WHERE n < 100 ORDER BY n DESC LIMIT 1;
----
analyzed_plan	<REGEX>:.*WHERE `n` < 100 ORDER BY `n` DESC NULLS LAST LIMIT 1".*

# plain LIMIT
query I
SELECT count(*) FROM (SELECT n FROM ch.test_db.big LIMIT 5);
----
5

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT n FROM ch.test_db.big LIMIT 5;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT `n` FROM `test_db`.`big` LIMIT 5".*

# not pushed: ORDER BY an expression
query I
SELECT id FROM ch.test_db.t1 ORDER BY id % 2, id LIMIT 1;
----
2

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.t1 ORDER BY id % 2, id LIMIT 1;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT [^"]*FROM `test_db`.`t1`".*

# not pushed: a filter DuckDB evaluates itself (the LIMIT must apply after it)
query I
SELECT id FROM ch.test_db.t1 WHERE value > 100 ORDER BY id LIMIT 1;
----
2

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.t1 WHERE value > 100 ORDER BY id LIMIT 1;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT [^"]*FROM `test_db`.`t1`".*

# not pushed: ordering on a float column
query I
SELECT id FROM ch.test_db.t1 ORDER BY value DESC LIMIT 1;
----
3

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT id FROM ch.test_db.t1 ORDER BY value DESC LIMIT 1;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT [^"]*FROM `test_db`.`t1`".*

statement ok
SET ch_order_pushdown = false;

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT n FROM ch.test_db.big ORDER BY n DESC LIMIT 3;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT `n` FROM `test_db`.`big`".*
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `make release && make smoke ARGS=test/sql/pushdown/limit.test`
Expected: FAIL on the first EXPLAIN (no `ORDER BY` in the generated query).

- [ ] **Step 3: Implement the optimizer**

`src/include/storage/clickhouse_optimizer.hpp`:
```cpp
#pragma once

#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

//! Moves LIMIT / OFFSET and ORDER BY ... LIMIT (TOP_N) that sit directly on top of a ClickHouse scan into the query
//! sent to ClickHouse
class ClickhouseOptimizer {
public:
	static void Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan);
};

} // namespace duckdb
```

`src/storage/clickhouse_optimizer.cpp`:
```cpp
#include "storage/clickhouse_optimizer.hpp"

#include "clickhouse_scanner.hpp"
#include "clickhouse_utils.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_limit.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_top_n.hpp"

namespace duckdb {

//! The ClickHouse scan directly below op, looking through projections
static optional_ptr<LogicalGet> FindClickhouseScan(LogicalOperator &op) {
	reference<LogicalOperator> current = op;
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		current = *current.get().children[0];
	}
	if (current.get().type != LogicalOperatorType::LOGICAL_GET) {
		return nullptr;
	}
	auto &get = current.get().Cast<LogicalGet>();
	if (!ClickhouseScanFunction::IsClickhouseScan(get.function.name) || !get.bind_data) {
		return nullptr;
	}
	return &get;
}

//! A LIMIT may only move into ClickHouse when ClickHouse also evaluates every filter of the scan
static bool AllFiltersPushed(LogicalGet &get, const ClickhouseScanBindData &bind_data) {
	for (auto &entry : get.table_filters.filters) {
		auto column_id = entry.first;
		if (IsVirtualColumn(column_id) || column_id >= bind_data.columns.size() || !bind_data.filter_pushdown ||
		    !ClickhouseTypes::SupportsPushdown(bind_data.columns[column_id].type_node)) {
			return false;
		}
	}
	return true;
}

//! The quoted ClickHouse column that expr refers to (through projections), or "" if it is not a plain,
//! pushdown-safe column of the scan
static string TraceColumn(Expression &expr, LogicalOperator &child, LogicalGet &get,
                          const ClickhouseScanBindData &bind_data) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return string();
	}
	auto &column_ref = expr.Cast<BoundColumnRefExpression>();
	if (column_ref.depth > 0) {
		return string();
	}
	auto binding = column_ref.binding;
	reference<LogicalOperator> current = child;
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = current.get().Cast<LogicalProjection>();
		if (binding.table_index != projection.table_index || binding.column_index >= projection.expressions.size()) {
			return string();
		}
		auto &projected = *projection.expressions[binding.column_index];
		if (projected.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return string();
		}
		auto &inner = projected.Cast<BoundColumnRefExpression>();
		if (inner.depth > 0) {
			return string();
		}
		binding = inner.binding;
		current = *current.get().children[0];
	}
	if (binding.table_index != get.table_index) {
		return string();
	}
	auto &column_ids = get.GetColumnIds();
	if (binding.column_index >= column_ids.size() || column_ids[binding.column_index].IsVirtualColumn()) {
		return string();
	}
	auto column_id = column_ids[binding.column_index].GetPrimaryIndex();
	if (column_id >= bind_data.columns.size()) {
		return string();
	}
	auto &column = bind_data.columns[column_id];
	if (!ClickhouseTypes::SupportsPushdown(column.type_node)) {
		return string();
	}
	return ClickhouseUtils::QuoteIdentifier(column.name);
}

static string BuildOrderByClause(vector<BoundOrderByNode> &orders, LogicalOperator &child, LogicalGet &get,
                                 const ClickhouseScanBindData &bind_data) {
	vector<string> keys;
	for (auto &order : orders) {
		auto column = TraceColumn(*order.expression, child, get, bind_data);
		if (column.empty()) {
			return string();
		}
		auto descending = order.type == OrderType::DESCENDING;
		auto nulls_first = order.null_order == OrderByNullType::NULLS_FIRST;
		keys.push_back(column + (descending ? " DESC" : " ASC") + (nulls_first ? " NULLS FIRST" : " NULLS LAST"));
	}
	return " ORDER BY " + StringUtil::Join(keys, ", ");
}

static void OptimizeRecursive(unique_ptr<LogicalOperator> &op) {
	if (op->type == LogicalOperatorType::LOGICAL_TOP_N) {
		auto &top_n = op->Cast<LogicalTopN>();
		auto get = FindClickhouseScan(*op->children[0]);
		if (get) {
			auto &bind_data = get->bind_data->Cast<ClickhouseScanBindData>();
			if (bind_data.limit_clause.empty() && AllFiltersPushed(*get, bind_data)) {
				auto order_by = BuildOrderByClause(top_n.orders, *op->children[0], *get, bind_data);
				if (!order_by.empty()) {
					bind_data.order_by_clause = order_by;
					bind_data.limit_clause = " LIMIT " + to_string(top_n.limit);
					if (top_n.offset > 0) {
						bind_data.limit_clause += " OFFSET " + to_string(top_n.offset);
					}
					op = std::move(op->children[0]);
					return;
				}
			}
		}
	} else if (op->type == LogicalOperatorType::LOGICAL_LIMIT) {
		auto &limit = op->Cast<LogicalLimit>();
		auto get = FindClickhouseScan(*op->children[0]);
		auto constant_limit = limit.limit_val.Type() == LimitNodeType::CONSTANT_VALUE;
		auto offset_type = limit.offset_val.Type();
		auto constant_offset = offset_type == LimitNodeType::CONSTANT_VALUE || offset_type == LimitNodeType::UNSET;
		if (get && constant_limit && constant_offset) {
			auto &bind_data = get->bind_data->Cast<ClickhouseScanBindData>();
			if (bind_data.limit_clause.empty() && AllFiltersPushed(*get, bind_data)) {
				bind_data.limit_clause = " LIMIT " + to_string(limit.limit_val.GetConstantValue());
				if (offset_type == LimitNodeType::CONSTANT_VALUE && limit.offset_val.GetConstantValue() > 0) {
					bind_data.limit_clause += " OFFSET " + to_string(limit.offset_val.GetConstantValue());
				}
				op = std::move(op->children[0]);
				return;
			}
		}
	}
	for (auto &child : op->children) {
		OptimizeRecursive(child);
	}
}

void ClickhouseOptimizer::Optimize(OptimizerExtensionInput &input, unique_ptr<LogicalOperator> &plan) {
	Value enabled;
	if (input.context.TryGetCurrentSetting("ch_order_pushdown", enabled) && !enabled.IsNull() &&
	    !BooleanValue::Get(enabled)) {
		return;
	}
	OptimizeRecursive(plan);
}

} // namespace duckdb
```

- [ ] **Step 4: Register the optimizer and the setting**

In `src/clickhouse_scanner_extension.cpp` add the includes `#include "duckdb/optimizer/optimizer_extension.hpp"` and `#include "storage/clickhouse_optimizer.hpp"`, and append to `LoadInternal`:
```cpp
	config.AddExtensionOption("ch_order_pushdown", "Push LIMIT and ORDER BY ... LIMIT down into ClickHouse queries",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	OptimizerExtension clickhouse_optimizer;
	clickhouse_optimizer.optimize_function = ClickhouseOptimizer::Optimize;
	OptimizerExtension::Register(config, std::move(clickhouse_optimizer));
```
Add `clickhouse_optimizer.cpp` to `clickhouse_ext_storage` in `src/storage/CMakeLists.txt`.

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make release && make smoke`
Expected: `All tests passed`. `test/sql/scan/basic.test` still covers early termination and cancellation, because its loop filters on an expression and so its LIMIT is not pushed.

- [ ] **Step 6: Commit**

```bash
git add src test
git commit -m "feat: push LIMIT and ORDER BY ... LIMIT down into ClickHouse

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---
### Task 9: `clickhouse_query()` and `clickhouse_scan()`

**Files:**
- Create: `src/clickhouse_query.cpp`
- Modify: `src/include/clickhouse_scanner.hpp`, `src/clickhouse_scanner.cpp`, `src/clickhouse_scanner_extension.cpp`, `src/CMakeLists.txt`
- Test: `test/sql/query/clickhouse_query.test`, `test/sql/query/clickhouse_scan.test`

**Interfaces:**
- Consumes: `ClickhouseScanBindData`, `ClickhouseScanFunction::{SetScanCallbacks, SetReturnTypes, FilterPushdownEnabled}`, `ClickhouseCatalog::{CATALOG_TYPE, GetConnectionPoolPtr}`, `ClickhouseSecrets`, `ClickhouseConnectionConfig`, `ClickhouseConnectionPool`, `ClickhouseColumnInfo::Create`.
- Produces:
  - SQL `clickhouse_query(database VARCHAR, sql VARCHAR)`.
  - SQL `clickhouse_scan(connection VARCHAR, database VARCHAR, table VARCHAR [, secret := VARCHAR])`.
  - C++ `class ClickhouseQueryFunction : public TableFunction`.

- [ ] **Step 1: Write the failing tests**

`test/sql/query/clickhouse_query.test`:
```
# name: test/sql/query/clickhouse_query.test
# description: clickhouse_query runs arbitrary ClickHouse SQL through an attached database
# group: [query]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

statement ok
ATTACH 'host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD} database=test_db' AS ch (TYPE clickhouse);

query II
SELECT * FROM clickhouse_query('ch', 'SELECT number AS n, toString(number) AS s FROM numbers(3)');
----
0	0
1	1
2	2

query I
SELECT * FROM clickhouse_query('ch', 'SELECT finalizeAggregation(total) AS total FROM test_db.aggregates');
----
10

# trailing semicolons are ignored; unqualified tables resolve in the connection's database
query I
SELECT u FROM clickhouse_query('ch', 'SELECT uniqExact(n) AS u FROM big;');
----
10000000

# the ClickHouse type mapping applies to query results
query II
SELECT typeof(ip), ip FROM clickhouse_query('ch', 'SELECT toIPv4(''1.2.3.4'') AS ip');
----
VARCHAR	1.2.3.4

# projection and filter pushdown wrap the query
query I
SELECT n FROM clickhouse_query('ch', 'SELECT number AS n, number * 2 AS m FROM numbers(100)') WHERE n = 42;
----
42

query II
EXPLAIN (ANALYZE, FORMAT JSON) SELECT n FROM clickhouse_query('ch', 'SELECT number AS n, number * 2 AS m FROM numbers(100)') WHERE n = 42;
----
analyzed_plan	<REGEX>:.*"ClickHouse Query": "SELECT `n` FROM \(SELECT number AS n, number \* 2 AS m FROM numbers\(100\)\) WHERE `n` = 42".*

query I
SELECT n FROM clickhouse_query('ch', 'SELECT number AS n FROM numbers(1000)') ORDER BY n DESC LIMIT 2;
----
999
998

statement error
SELECT * FROM clickhouse_query('nope', 'SELECT 1');
----
Failed to find attached database "nope" referenced in clickhouse_query

statement error
SELECT * FROM clickhouse_query('memory', 'SELECT 1');
----
Attached database "memory" is not a ClickHouse database

statement error
SELECT * FROM clickhouse_query('ch', 'SELECT * FROM does_not_exist');
----
ClickHouse error 60 (UNKNOWN_TABLE)

statement error
SELECT * FROM clickhouse_query('ch', NULL);
----
Parameters to clickhouse_query cannot be NULL
```

`test/sql/query/clickhouse_scan.test`:
```
# name: test/sql/query/clickhouse_scan.test
# description: clickhouse_scan reads a table without ATTACH
# group: [query]

require clickhouse_scanner

require-env CLICKHOUSE_TEST_HOST

require-env CLICKHOUSE_TEST_PORT

require-env CLICKHOUSE_TEST_USER

require-env CLICKHOUSE_TEST_PASSWORD

query II
SELECT id, name FROM clickhouse_scan('host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD}', 'test_db', 't1') ORDER BY id;
----
1	Alice
2	Bob
3	Charlie

query I
SELECT name FROM clickhouse_scan('host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD}', 'test_db', 't1') WHERE id = 3;
----
Charlie

statement ok
CREATE SECRET scan_secret (TYPE clickhouse, USER '${CLICKHOUSE_TEST_USER}', PASSWORD '${CLICKHOUSE_TEST_PASSWORD}');

query I
SELECT count(*) FROM clickhouse_scan('host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT}', 'test_db', 'big', secret := 'scan_secret');
----
10000000

statement error
SELECT * FROM clickhouse_scan('host=${CLICKHOUSE_TEST_HOST} port=${CLICKHOUSE_TEST_PORT} user=${CLICKHOUSE_TEST_USER} password=${CLICKHOUSE_TEST_PASSWORD}', 'test_db', 'nope');
----
ClickHouse table "test_db"."nope" not found
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `make release && make smoke ARGS='test/sql/query/*'`
Expected: FAIL with `Table Function with name clickhouse_query does not exist!`

- [ ] **Step 3: `clickhouse_scan` bind**

In `src/include/clickhouse_scanner.hpp`, after `ClickhouseScanFunction`, add:
```cpp
//! clickhouse_query('<attached database>', '<ClickHouse SQL>')
class ClickhouseQueryFunction : public TableFunction {
public:
	ClickhouseQueryFunction();
};
```

In `src/clickhouse_scanner.cpp` add the includes `#include "clickhouse_secrets.hpp"` and `#include "duckdb/common/exception.hpp"`. Add this bind function above the `ClickhouseScanFunction` constructor:
```cpp
static unique_ptr<FunctionData> ClickhouseScanBind(ClientContext &context, TableFunctionBindInput &input,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	for (auto &value : input.inputs) {
		if (value.IsNull()) {
			throw BinderException("Parameters to clickhouse_scan cannot be NULL");
		}
	}
	string secret_name;
	auto secret_parameter = input.named_parameters.find("secret");
	if (secret_parameter != input.named_parameters.end()) {
		secret_name = secret_parameter->second.ToString();
	}
	ClickhouseConnectionConfig config;
	auto secret_entry = ClickhouseSecrets::GetSecretEntry(context, secret_name);
	if (secret_entry) {
		ClickhouseSecrets::ApplySecret(*secret_entry, config);
	}
	config.ApplyConnectionString(StringValue::Get(input.inputs[0]));

	// a private pool for this scan; no background reaper thread for such a short-lived pool
	auto pool_config = ClickhouseConnectionPool::PoolConfigFromContext(context);
	pool_config.start_reaper_thread = false;
	auto result = make_uniq<ClickhouseScanBindData>();
	result->pool = make_shared_ptr<ClickhouseConnectionPool>(config, ClickhouseTimeouts::FromContext(context),
	                                                         pool_config);
	result->database = StringValue::Get(input.inputs[1]);
	result->table = StringValue::Get(input.inputs[2]);
	result->filter_pushdown = ClickhouseScanFunction::FilterPushdownEnabled(context);

	auto connection = result->pool->GetConnection();
	auto sql = "SELECT name, type FROM system.columns WHERE database = " +
	           ClickhouseUtils::QuoteLiteral(result->database) +
	           " AND table = " + ClickhouseUtils::QuoteLiteral(result->table) +
	           " AND default_kind != 'EPHEMERAL' ORDER BY position";
	for (auto &block : connection->Query(sql)) {
		auto column_names = block[0]->As<clickhouse::ColumnString>();
		auto column_types = block[1]->As<clickhouse::ColumnString>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			result->columns.push_back(
			    ClickhouseColumnInfo::Create(string(column_names->At(row)), string(column_types->At(row))));
		}
	}
	if (result->columns.empty()) {
		throw BinderException("ClickHouse table \"%s\".\"%s\" not found", result->database, result->table);
	}
	ClickhouseScanFunction::SetReturnTypes(*result, return_types, names);
	return std::move(result);
}
```
Replace the `ClickhouseScanFunction` constructor with:
```cpp
ClickhouseScanFunction::ClickhouseScanFunction()
    : TableFunction("clickhouse_scan", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
                    ClickhouseScan, ClickhouseScanBind) {
	SetScanCallbacks(*this);
	named_parameters["secret"] = LogicalType::VARCHAR;
}
```

- [ ] **Step 4: `clickhouse_query`**

`src/clickhouse_query.cpp`:
```cpp
#include "clickhouse_scanner.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "storage/clickhouse_catalog.hpp"

namespace duckdb {

static string StripTrailingSemicolons(string sql) {
	StringUtil::RTrim(sql);
	while (!sql.empty() && sql.back() == ';') {
		sql.pop_back();
		StringUtil::RTrim(sql);
	}
	return sql;
}

static unique_ptr<FunctionData> ClickhouseQueryBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	if (input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		throw BinderException("Parameters to clickhouse_query cannot be NULL");
	}
	auto database_name = StringValue::Get(input.inputs[0]);
	auto database = DatabaseManager::Get(context).GetDatabase(context, database_name);
	if (!database) {
		throw BinderException("Failed to find attached database \"%s\" referenced in clickhouse_query",
		                      database_name);
	}
	auto &catalog = database->GetCatalog();
	if (catalog.GetCatalogType() != ClickhouseCatalog::CATALOG_TYPE) {
		throw BinderException("Attached database \"%s\" is not a ClickHouse database", database_name);
	}

	auto result = make_uniq<ClickhouseScanBindData>();
	result->pool = catalog.Cast<ClickhouseCatalog>().GetConnectionPoolPtr();
	result->query = StripTrailingSemicolons(StringValue::Get(input.inputs[1]));
	if (result->query.empty()) {
		throw BinderException("clickhouse_query: the query cannot be empty");
	}
	result->filter_pushdown = ClickhouseScanFunction::FilterPushdownEnabled(context);

	// DESCRIBE gives the result columns without running the query
	auto connection = result->pool->GetConnection();
	unordered_set<string> seen_names;
	for (auto &block : connection->Query("DESCRIBE TABLE (" + result->query + ")")) {
		auto column_names = block[0]->As<clickhouse::ColumnString>();
		auto column_types = block[1]->As<clickhouse::ColumnString>();
		for (size_t row = 0; row < block.GetRowCount(); row++) {
			string name(column_names->At(row));
			if (!seen_names.insert(name).second) {
				throw BinderException("clickhouse_query: the query returns more than one column named \"%s\"; give "
				                      "its columns unique aliases",
				                      name);
			}
			result->columns.push_back(ClickhouseColumnInfo::Create(name, string(column_types->At(row))));
		}
	}
	if (result->columns.empty()) {
		throw BinderException("clickhouse_query: the query does not return any columns");
	}
	ClickhouseScanFunction::SetReturnTypes(*result, return_types, names);
	return std::move(result);
}

ClickhouseQueryFunction::ClickhouseQueryFunction()
    : TableFunction("clickhouse_query", {LogicalType::VARCHAR, LogicalType::VARCHAR}, nullptr, ClickhouseQueryBind) {
	ClickhouseScanFunction::SetScanCallbacks(*this);
}

} // namespace duckdb
```
Because the wrapped query is used as a subquery (`SELECT … FROM (<sql>)`), projection, filter and LIMIT pushdown apply to it too.

- [ ] **Step 5: Register both functions**

In `src/clickhouse_scanner_extension.cpp` add `#include "clickhouse_scanner.hpp"` and, next to the other `RegisterFunction` calls:
```cpp
	loader.RegisterFunction(ClickhouseScanFunction());
	loader.RegisterFunction(ClickhouseQueryFunction());
```
Add `clickhouse_query.cpp` to `clickhouse_ext` in `src/CMakeLists.txt`.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `make release && make smoke`
Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src test
git commit -m "feat: add clickhouse_query() and clickhouse_scan() table functions

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 10: CI workflows and README

**Files:**
- Create: `.github/workflows/MainDistributionPipeline.yml`, `.github/workflows/IntegrationTests.yml`, `README.md`, `LICENSE`

**Interfaces:**
- Consumes: `make release`, `make smoke` (Task 2).

- [ ] **Step 1: Distribution pipeline** (builds every community platform; server tests skip themselves)

`.github/workflows/MainDistributionPipeline.yml`:
```yaml
#
# Builds (and on tags, deploys) the extension for all DuckDB platforms via extension-ci-tools
#
name: Main Extension Distribution Pipeline
on:
  push:
  pull_request:
  workflow_dispatch:

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}-${{ github.head_ref || '' }}-${{ github.base_ref || '' }}-${{ github.ref != 'refs/heads/main' && github.sha || '' }}
  cancel-in-progress: true

jobs:
  duckdb-stable-build:
    name: Build extension binaries
    uses: duckdb/extension-ci-tools/.github/workflows/_extension_distribution.yml@v1.5-variegata
    with:
      duckdb_version: v1.5.5
      ci_tools_version: v1.5-variegata
      extension_name: clickhouse_scanner
      exclude_archs: 'wasm_mvp;wasm_eh;wasm_threads;windows_amd64_mingw'
```

- [ ] **Step 2: Integration tests against a real ClickHouse**

`.github/workflows/IntegrationTests.yml`:
```yaml
name: Integration Tests
on:
  push:
  pull_request:
  workflow_dispatch:

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}-${{ github.head_ref || '' }}-${{ github.base_ref || '' }}-${{ github.ref != 'refs/heads/main' || github.sha }}
  cancel-in-progress: true

jobs:
  linux-tests:
    name: Linux tests against ClickHouse
    runs-on: ubuntu-latest
    env:
      GEN: ninja
      VCPKG_TARGET_TRIPLET: x64-linux-release
      VCPKG_HOST_TRIPLET: x64-linux-release
      VCPKG_TOOLCHAIN_PATH: ${{ github.workspace }}/vcpkg/scripts/buildsystems/vcpkg.cmake

    steps:
      - name: Checkout
        uses: actions/checkout@v4
        with:
          fetch-depth: 0
          submodules: 'true'

      - name: Install dependencies
        run: |
          sudo apt-get update -y -q
          sudo apt-get install -y -q build-essential ninja-build

      - name: Setup vcpkg
        uses: lukka/run-vcpkg@v11.1
        with:
          vcpkgGitCommitId: 84bab45d415d22042bd0b9081aea57f362da3f35

      - name: Build extension
        run: make release

      # same entry point as locally; not a service container, because the TLS config and test
      # certificates have to be mounted from the checkout
      - name: Smoke tests against ClickHouse
        run: make smoke
```

- [ ] **Step 3: README**

`README.md`:
````markdown
# DuckDB ClickHouse extension (`clickhouse_scanner`)

Query [ClickHouse](https://clickhouse.com) from DuckDB. The extension attaches a ClickHouse service, including
ClickHouse Cloud, as a read-only DuckDB database. It speaks the native protocol (with TLS) and pushes projections,
filters and `LIMIT` / `ORDER BY … LIMIT` down into ClickHouse.

```sql
INSTALL clickhouse_scanner FROM community;
LOAD clickhouse_scanner;

CREATE SECRET ch (TYPE clickhouse, HOST 'abc123.eu-west-1.aws.clickhouse.cloud', PORT 9440,
                  USER 'default', PASSWORD '...', DATABASE 'analytics');
ATTACH '' AS ch (TYPE clickhouse, SECRET ch);

SELECT event, count(*) FROM ch.analytics.events WHERE ts > now() - INTERVAL 1 DAY GROUP BY ALL;
```

## Connecting

`ATTACH '<connection>' AS name (TYPE clickhouse [, SECRET name] [, SETTINGS 'k=v,…'] [, SHOW_SYSTEM true])`

`<connection>` is either `key=value` pairs (`host=localhost port=9000 user=default password='p w'`) or a URI
(`clickhouse://user:password@host:9000/database`, or `clickhouses://…` for TLS). Settings are applied in this
order: the secret (or the unnamed `clickhouse` secret), then the connection string.

| Option        | Default                    | Description                                               |
|---------------|----------------------------|-----------------------------------------------------------|
| `host`        | `localhost`                | Server host name                                          |
| `port`        | `9000`, or `9440` if secure | Native protocol port                                      |
| `user`        | `default`                  | User name                                                 |
| `password`    |                            | Password (never shown in errors or `duckdb_databases()`)  |
| `database`    | `default`                  | Default schema of the attached database                   |
| `secure`      | `true` when port is 9440   | Use TLS                                                   |
| `ca_cert`     | system CA bundle           | PEM file with the CA certificates to trust                |
| `skip_verify` | `false`                    | Do not verify the server certificate (testing only)       |
| `compression` | `lz4`                      | `lz4`, `zstd` or `none`                                   |
| `settings`    |                            | ClickHouse settings sent with every query: `k1=v1,k2=v2`  |

Each ClickHouse database becomes a DuckDB schema. `system`, `INFORMATION_SCHEMA` and `information_schema` are hidden
unless you set `SHOW_SYSTEM true`. Metadata is cached; `CALL clickhouse_clear_cache()` refreshes it.

On Windows there is no system CA bundle that OpenSSL can read, so pass `ca_cert` when using TLS.

## Functions

| Function | Description |
|---|---|
| `clickhouse_query(database, sql)` | Runs any ClickHouse query through an attached database. Projections, filters and LIMIT are pushed into it. |
| `clickhouse_scan(connection, database, table [, secret := name])` | Reads one table without `ATTACH`. |
| `clickhouse_clear_cache()` | Forgets cached databases, tables and columns. |
| `clickhouse_type_mapping(type)` | Shows how a ClickHouse type is mapped and read. |

## Settings

| Setting | Default | Description |
|---|---|---|
| `ch_filter_pushdown` | `true` | Push filters into ClickHouse queries |
| `ch_order_pushdown` | `true` | Push `LIMIT` and `ORDER BY … LIMIT` into ClickHouse queries |
| `ch_connect_timeout_ms` | `10000` | Connection timeout |
| `ch_receive_timeout_ms` | `300000` | Socket receive timeout |
| `ch_pool_max_connections` | depends on CPU count | Connection pool size per attached database (new ATTACHes) |
| `ch_pool_acquire_mode` | `force` | `force`, `wait` or `try` when the pool is exhausted |
| `ch_pool_wait_timeout_millis` | `30000` | Wait limit for `wait` mode |
| `ch_pool_idle_timeout_millis` | `60000` | Idle connections are closed after this long |
| `ch_debug_show_queries` | `false` | Print every query sent to ClickHouse |

`EXPLAIN ANALYZE` shows the exact query sent to ClickHouse (`ClickHouse Query`).

## Type mapping

| ClickHouse | DuckDB |
|---|---|
| `Bool`, `(U)Int8…64`, `Int128`, `UInt128`, `Float32/64` | matching integer / `HUGEINT` / `UHUGEINT` / `FLOAT` / `DOUBLE` |
| `Decimal(P ≤ 38, S)` | `DECIMAL(P, S)` |
| `String`, `FixedString` | `VARCHAR` (must be valid UTF-8) |
| `Date`, `Date32` | `DATE` |
| `DateTime`, `DateTime64` | `TIMESTAMP WITH TIME ZONE` (microseconds) |
| `Time`, `Time64` | `TIME` / `TIME_NS` |
| `UUID` | `UUID` |
| `Enum8/16` | `ENUM` |
| `Array`, `Tuple`, `Map` | `LIST`, `STRUCT`, `MAP` |
| `JSON`, `Variant`, `Dynamic` | `JSON` |
| `Nullable(T)`, `LowCardinality(T)`, `SimpleAggregateFunction(f, T)` | `T` |
| `IPv4/6`, `(U)Int256`, `Decimal256`, geo types, others | `VARCHAR` (converted by ClickHouse) |
| `AggregateFunction` | not readable; use `clickhouse_query` with `finalizeAggregation` |

## Limitations

- Read-only: `INSERT`, `UPDATE`, `DELETE` and DDL are rejected.
- ClickHouse has no multi-statement transactions. Two scans in one DuckDB transaction may see different data.
- Filters on floating-point, UUID, `FixedString`, `Time` and nested columns are evaluated by DuckDB, not ClickHouse.
- Not available in DuckDB-WASM (the native protocol needs TCP).

## Development

```bash
git submodule update --init --recursive
export VCPKG_TOOLCHAIN_PATH=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja
make release
make test                                  # tests that need no server
make smoke                                 # all tests against a throw-away ClickHouse 25.8 container (Docker)
make smoke ARGS=test/sql/scan/scalars.test # a single test file
```

The clickhouse-cpp vcpkg port is adapted from [pixonic/duckdb-clickhouse](https://github.com/pixonic/duckdb-clickhouse)
(MIT). The design follows [duckdb/duckdb-postgres](https://github.com/duckdb/duckdb-postgres).

## License

MIT
````

`LICENSE`: the MIT license text with `Copyright (c) 2026 Altertable`.

- [ ] **Step 4: Verify workflows parse and the full suite passes**

```bash
python3 -c "import yaml,sys; [yaml.safe_load(open(f)) for f in sys.argv[1:]]" .github/workflows/*.yml
make release && make test && make smoke
```
Expected: no YAML error, and `All tests passed` twice.

- [ ] **Step 5: Commit**

```bash
git add .github README.md LICENSE
git commit -m "ci: distribution and ClickHouse integration workflows; README

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Self-review checklist (for the plan author; already applied)

- Spec coverage. ATTACH/secrets/URI/TLS: Task 4. Catalog mapping, system databases, clear cache, read-only: Tasks 4–6. Type mapping: Tasks 3, 5, 6. Parallel pull scan and cancellation: Task 6. Filter pushdown: Task 7. LIMIT / TOP-N: Task 8. `clickhouse_query` and `clickhouse_scan`: Task 9. Settings: Tasks 4, 7, 8. `make smoke` (throw-away container, TLS, fixtures) and CI: Tasks 2 and 10. README: Task 10.
- Deliberately out of scope (spec section 7): writes, aggregate pushdown, multi-stream scans, HTTP transport, WASM, `clickhouse_configure_pool`.
