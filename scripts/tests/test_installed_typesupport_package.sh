#!/usr/bin/env bash
# Validate a generated TypeSupport sidecar through the installed ABI boundary.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE="$SCRIPT_DIR/fixtures/installed_typesupport"
if [[ -n "${IBMW_INSTALLED_TYPESUPPORT_SCRATCH:-}" ]]; then
  SCRATCH="$IBMW_INSTALLED_TYPESUPPORT_SCRATCH"
  mkdir -p "$SCRATCH"
else
  SCRATCH="$(mktemp -d)"
  if [[ "${IBMW_INSTALLED_TYPESUPPORT_KEEP:-0}" != 1 ]]; then
    trap 'rm -rf "$SCRATCH"' EXIT
  fi
fi

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
note() { printf '[installed-typesupport] %s\n' "$*"; }

for command in cmake python3 ldd; do
  command -v "$command" >/dev/null || fail "$command is required"
done

PREBUILT_IBMW="${IBMW_INSTALLED_TYPESUPPORT_IBMW_BUILD:-}"
IBMW_BUILD="${PREBUILT_IBMW:-$SCRATCH/ibmw-build}"
IBMW_PREFIX="$SCRATCH/ibmw-prefix"
GENERATED="$SCRATCH/generated"
GENERATED_BUILD="$SCRATCH/generated-build"
GENERATED_PREFIX="$SCRATCH/generated-prefix"
LOADER_BUILD="$SCRATCH/loader-build"
LOADER_LINK_COVERAGE="${IBMW_INSTALLED_TYPESUPPORT_LOADER_LINK_COVERAGE:-0}"

if [[ -n "$PREBUILT_IBMW" ]]; then
  [[ -f "$IBMW_BUILD/CMakeCache.txt" ]] ||
    fail "prebuilt IBMW directory does not contain CMakeCache.txt: $IBMW_BUILD"
  note "installing prebuilt IBMW from $IBMW_BUILD to $IBMW_PREFIX"
else
  note "building IBMW for installation to $IBMW_PREFIX"
  cmake -S "$REPO_ROOT" -B "$IBMW_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$IBMW_PREFIX" \
    -DIBMW_INSTALL=ON \
    -DIBMW_BUILD_RUNTIME=ON \
    -DIBMW_BUILD_PROTOCOLS=OFF \
    -DIBMW_BUILD_TESTS=OFF \
    -DIBMW_BUILD_EXAMPLES=OFF \
    -DIBMW_BUILD_WITH_WERROR=OFF >/dev/null
  cmake --build "$IBMW_BUILD" --parallel "${IBMW_TEST_JOBS:-2}" >/dev/null
fi
cmake --install "$IBMW_BUILD" --prefix "$IBMW_PREFIX" >/dev/null

# The installed ibmw config resolves its dependencies with find_dependency;
# forward the directories IBMW itself was configured with, so the test does
# not depend on CMAKE_PREFIX_PATH in the caller's environment (same as
# test_installed_coro_package.sh).
DEP_HINTS=()
for dep in fastcdr fastrtps fastdds foonathan_memory; do
  dir="$(sed -n "s/^${dep}_DIR:[A-Z]*=//p" "$IBMW_BUILD/CMakeCache.txt" | head -n1)"
  if [[ -n "$dir" && "$dir" != *-NOTFOUND ]]; then DEP_HINTS+=("-D${dep}_DIR=$dir"); fi
done

note "generating and installing sidecar"
python3 "$REPO_ROOT/src/tools/ibmw_gen/ibmw_gen.py" \
  "$FIXTURE/installed_typesupport.ibmw.yaml" -o "$GENERATED" >/dev/null
cmake -S "$GENERATED" -B "$GENERATED_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$IBMW_PREFIX" \
  -DCMAKE_INSTALL_PREFIX="$GENERATED_PREFIX" "${DEP_HINTS[@]}" >/dev/null
cmake --build "$GENERATED_BUILD" --parallel "${IBMW_TEST_JOBS:-2}" >/dev/null
cmake --install "$GENERATED_BUILD" >/dev/null

SIDECAR="$GENERATED_PREFIX/lib/libinstalled_typesupport_json_ts.so"
[[ -f "$SIDECAR" ]] || fail "generated sidecar was not installed"
if ldd "$SIDECAR" | grep -q 'not found'; then
  ldd "$SIDECAR" >&2
  fail "generated sidecar has unresolved dynamic dependencies"
fi

note "building independent installed loader"
LOADER_CMAKE_ARGS=()
if [[ "$LOADER_LINK_COVERAGE" == 1 ]]; then
  LOADER_CMAKE_ARGS+=("-DCMAKE_EXE_LINKER_FLAGS=--coverage")
fi
cmake -S "$FIXTURE" -B "$LOADER_BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$IBMW_PREFIX" \
  "${DEP_HINTS[@]}" "${LOADER_CMAKE_ARGS[@]}" >/dev/null
cmake --build "$LOADER_BUILD" --parallel "${IBMW_TEST_JOBS:-2}" >/dev/null
LD_LIBRARY_PATH="$GENERATED_PREFIX/lib:$IBMW_PREFIX/lib:${LD_LIBRARY_PATH:-}" \
  "$LOADER_BUILD/installed_typesupport_loader" "$SIDECAR"

note "PASS: generate, build, install, dependency resolution, and independent load"
