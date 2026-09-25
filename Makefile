PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=clickhouse_scanner
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Upstream tidy-check does not pass the vcpkg manifest, only matches sources nested
# under src/, and cannot see the Apple SDK. Drop that target and define it below.
# The filtered makefile is regenerated only when extension-ci-tools changes.
$(shell mkdir -p .cache && awk 'BEGIN{skip=0} /^tidy-check:/{skip=1; next} skip && /^[^#[:space:]]/{skip=0} skip{next} {print}' extension-ci-tools/makefiles/duckdb_extension.Makefile > .cache/duckdb_extension.Makefile.tmp && if cmp -s .cache/duckdb_extension.Makefile.tmp .cache/duckdb_extension.Makefile; then rm -f .cache/duckdb_extension.Makefile.tmp; else mv .cache/duckdb_extension.Makefile.tmp .cache/duckdb_extension.Makefile; fi)

include .cache/duckdb_extension.Makefile

#### Tests against a throw-away ClickHouse container (see scripts/test_with_clickhouse.sh)
SMOKE_BUILD ?= release
ARGS ?=

.PHONY: smoke
smoke:
	set -f; SMOKE_BUILD=$(SMOKE_BUILD) ./scripts/test_with_clickhouse.sh $(ARGS)

# Apple Clang omits the SDK sysroot from compile_commands.json. Homebrew clang-tidy does not infer it.
ifeq ($(shell uname -s),Darwin)
TIDY_EXTRA_ARG_PARAMETER := -extra-arg=-isysroot$(shell xcrun --show-sdk-path) -extra-arg=-stdlib=libc++
endif

tidy-check:
	mkdir -p ./build/tidy
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DDISABLE_UNITY=1 -DCLANG_TIDY=1 -S $(DUCKDB_SRCDIR) -B build/tidy
	cp duckdb/.clang-tidy build/tidy/.clang-tidy
	cd build/tidy && python3 ../../duckdb/scripts/run-clang-tidy.py '$(PROJ_DIR)src/.*' -header-filter '$(PROJ_DIR)src/.*' -quiet ${TIDY_THREAD_PARAMETER} ${TIDY_BINARY_PARAMETER} ${TIDY_PERFORM_CHECKS} ${TIDY_EXTRA_ARG_PARAMETER}
