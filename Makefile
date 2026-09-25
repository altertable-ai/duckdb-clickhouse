PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=clickhouse_scanner
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

#### Tests against a throw-away ClickHouse container (see scripts/test_with_clickhouse.sh)
SMOKE_BUILD ?= release
ARGS ?=

.PHONY: smoke
smoke:
	set -f; SMOKE_BUILD=$(SMOKE_BUILD) ./scripts/test_with_clickhouse.sh $(ARGS)
