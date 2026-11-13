#!/usr/bin/env bash
# Real install-tree consumer, Reporter ownership, and ABI gate.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE="$SCRIPT_DIR/fixtures/installed_consumer"
SCRATCH="${IBMW_INSTALLED_CONSUMER_SCRATCH:-$(mktemp -d)}"
KEEP="${IBMW_INSTALLED_CONSUMER_KEEP:-0}"
if [[ "$KEEP" != 1 ]]; then trap 'rm -rf "$SCRATCH"' EXIT; fi

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
note() { printf '[installed-consumer] %s\n' "$*"; }
contains_forbidden() {
  local path="$1"
  grep -R -F -e "$REPO_ROOT" -e "$SCRATCH/build-" "$path" >/dev/null 2>&1
}
assert_no_forbidden_target_properties() {
  local cmake_files="$1"
  local property
  for property in INTERFACE_INCLUDE_DIRECTORIES IMPORTED_LOCATION IMPORTED_LOCATION_RELEASE; do
    grep -E "${property}.*(${REPO_ROOT}|${SCRATCH}/build-)" "$cmake_files" >/dev/null 2>&1 &&
      fail "installed ${property} points to source/build"
  done
  return 0
}

command -v cmake >/dev/null || fail "cmake is required"
command -v readelf >/dev/null || fail "readelf is required"
command -v nm >/dev/null || fail "nm is required"

run_lane() {
  local lane="$1" reporter="$2" deepbind="$3"
  local build="$SCRATCH/build-$lane" prefix="$SCRATCH/prefix-$lane"
  local consumer_build="$SCRATCH/consumer-$lane"
  note "$lane prefix=$prefix"

  cmake -S "$REPO_ROOT" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DIBMW_INSTALL=ON \
    -DIBMW_BUILD_RUNTIME=ON -DIBMW_BUILD_PROTOCOLS=OFF \
    -DIBMW_BUILD_TESTS=OFF -DIBMW_BUILD_EXAMPLES=OFF \
    -DIBMW_BUILD_WITH_WERROR=OFF -DIBMW_ENABLE_CORE_METRICS="$reporter" \
    -DIBMW_ENABLE_REPORTER_V1="$reporter" \
    -DIBMW_ENABLE_DLOPEN_DEEPBIND="$deepbind" >/dev/null
  cmake --build "$build" --parallel "${IBMW_TEST_JOBS:-2}" >/dev/null
  cmake --install "$build" >/dev/null

  [[ -f "$prefix/include/ibmw/extension_api/ibmw_reporter_v1.h" ]] ||
    fail "$lane did not install Reporter header"
  contains_forbidden "$prefix/lib/cmake/ibmw" &&
    fail "$lane installed CMake package points to source/build"
  assert_no_forbidden_target_properties "$prefix/lib/cmake/ibmw"

  env -u ibmw_DIR -u CMAKE_PREFIX_PATH cmake -S "$FIXTURE" -B "$consumer_build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$prefix" \
    -DIBMW_EXPECT_REPORTER="$reporter" >/dev/null
  contains_forbidden "$consumer_build/CMakeCache.txt" &&
    fail "$lane consumer cache references source/build"
  cmake --build "$consumer_build" --parallel "${IBMW_TEST_JOBS:-2}" >/dev/null

  "$consumer_build/reporter_c11"
  "$consumer_build/abi_golden"
  "$consumer_build/installed_consumer"

  local host="$consumer_build/installed_consumer"
  local plugin="$consumer_build/libreporter_plugin.so"
  local runtime="$prefix/bin/ibmw"
  readelf -Ws "$host" | grep -E 'GLOBAL +DEFAULT +[0-9]+ +ibmw_reporter_query_v1$' >/dev/null ||
    fail "$lane runtime host does not export ibmw_reporter_query_v1"
  nm -D --defined-only "$plugin" | grep -q 'ibmw_reporter_query_v1' &&
    fail "$lane plugin defines reporter query"
  readelf -Ws "$runtime" | grep -E 'GLOBAL +DEFAULT +[0-9]+ +ibmw_reporter_query_v1$' >/dev/null ||
    fail "$lane installed runtime does not export ibmw_reporter_query_v1"
  readelf -d "$host" "$plugin" | grep -E "(RPATH|RUNPATH).*(build-|${REPO_ROOT})" >/dev/null &&
    fail "$lane binary RPATH points to source/build"
  note "$lane symbols: host=$(nm -D --defined-only "$host" | grep ' ibmw_reporter_query_v1$') plugin-query-definitions=0"
}

mkdir -p "$SCRATCH"
FIXTURE_COPY="$SCRATCH/fixture"
cmake -E copy_directory "$FIXTURE" "$FIXTURE_COPY"
FIXTURE="$FIXTURE_COPY"
run_lane off OFF OFF
run_lane on ON ON

note "prefixes: $SCRATCH/prefix-off $SCRATCH/prefix-on"
note "PASS: real Release install consumers, C11 ABI, tri-state query, deepbind ON/OFF, ownership, paths, and ABI golden"
