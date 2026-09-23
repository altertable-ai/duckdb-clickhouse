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

# Runs the unittest binary, streaming its output to the terminal while also capturing it so we
# can detect the "No tests ran" case (e.g. a typo'd or stale ARGS filter). The Catch2-based
# unittest binary exits 0 for that case, which would otherwise report success with zero tests run.
run_unittest() {
  local output_file
  output_file="$(mktemp)"
  set +e
  "${UNITTEST}" "$@" 2>&1 | tee "${output_file}"
  local status="${PIPESTATUS[0]}"
  set -e

  if grep -q "No tests ran" "${output_file}"; then
    rm -f "${output_file}"
    echo "ERROR: no tests matched the given filter ('$*'); treating as a failure." >&2
    exit 1
  fi
  rm -f "${output_file}"
  return "${status}"
}

echo "==> Running ${BUILD} tests against ${CLICKHOUSE_TEST_HOST}:${CLICKHOUSE_TEST_PORT} (TLS ${CLICKHOUSE_TEST_TLS_PORT}) ..."
if [[ $# -gt 0 ]]; then
  run_unittest "$@"
else
  run_unittest "test/*"
fi
