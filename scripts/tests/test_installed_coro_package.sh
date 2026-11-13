#!/usr/bin/env bash
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Installed-tree check for libibmw_selfdev_coro:
#   1. bin/ibmw records DT_NEEDED libibmw_selfdev_coro.so.<SOVERSION> and
#      RUNPATH $ORIGIN/../lib, so the library is loaded at startup.
#   2. A user package built with find_package(ibmw) + ibmw_add_pkg gets a
#      RUNPATH relative to $ORIGIN by default, without any absolute path. It
#      finds <prefix>/lib, also after both trees moved together.
#   3. The same package built with CMAKE_SKIP_RPATH=ON (no RUNPATH at all)
#      still loads and runs coroutines through bin/ibmw, because ibmw already
#      loaded the library and the dynamic loader matches it by SONAME.
#   4. With the selfdev crash handler (IBMW_SELFDEV_CRASH_HANDLER=ON), a
#      crash in package code (stack overflow, null store) run through
#      bin/ibmw with enable_crash_log gives the full report, with the package
#      module named in the header and the raw frames: the handler is installed
#      before the packages are loaded, and ModuleBus refreshes its module
#      table after loading them.
# All runs clear LD_LIBRARY_PATH.
#
# Env: IBMW_INSTALLED_CORO_IBMW_BUILD  configured+built IBMW build dir (required)
#      IBMW_INSTALLED_CORO_SCRATCH     work dir (default <build>/installed_coro_package_test);
#                                      reused across runs, never deleted by this script.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURE="$SCRIPT_DIR/fixtures/installed_coro_package"

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
note() { printf '[installed-coro] %s\n' "$*"; }

for command in cmake readelf ldd; do
  command -v "$command" >/dev/null || fail "$command is required"
done

IBMW_BUILD="${IBMW_INSTALLED_CORO_IBMW_BUILD:-}"
[[ -n "$IBMW_BUILD" && -f "$IBMW_BUILD/CMakeCache.txt" ]] ||
  fail "IBMW_INSTALLED_CORO_IBMW_BUILD must point to a configured IBMW build"
SCRATCH="${IBMW_INSTALLED_CORO_SCRATCH:-$IBMW_BUILD/installed_coro_package_test}"
mkdir -p "$SCRATCH"
PREFIX="$SCRATCH/ibmw-prefix"
JOBS="${IBMW_TEST_JOBS:-2}"

note "installing $IBMW_BUILD to $PREFIX"
cmake --install "$IBMW_BUILD" --prefix "$PREFIX" >/dev/null

IBMW_BIN="$PREFIX/bin/ibmw"
[[ -x "$IBMW_BIN" ]] || fail "bin/ibmw was not installed"
CORO_LIB="$(ls "$PREFIX"/lib/libibmw_selfdev_coro.so.* 2>/dev/null | head -n1 || true)"
[[ -n "$CORO_LIB" ]] || fail "versioned libibmw_selfdev_coro.so.* was not installed"
[[ -e "$PREFIX/lib/libibmw_selfdev_coro.so" ]] || fail "libibmw_selfdev_coro.so dev symlink missing"

SONAME="$(readelf -d "$CORO_LIB" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')"
[[ "$SONAME" == libibmw_selfdev_coro.so.* ]] || fail "unexpected SONAME '$SONAME'"
note "SONAME $SONAME"

IBMW_DYN="$(readelf -d "$IBMW_BIN")"
grep -q "(NEEDED).*\[$SONAME\]" <<<"$IBMW_DYN" ||
  { printf '%s\n' "$IBMW_DYN" >&2; fail "bin/ibmw has no DT_NEEDED $SONAME"; }
grep -q '(RUNPATH\|RPATH).*\$ORIGIN/\.\./lib' <<<"$IBMW_DYN" ||
  { printf '%s\n' "$IBMW_DYN" >&2; fail "bin/ibmw has no \$ORIGIN/../lib run path"; }
note "bin/ibmw NEEDED $SONAME, run path \$ORIGIN/../lib"

# ibmw-config.cmake re-finds the external packages IBMW was built against;
# forward the locations the IBMW build used (e.g. fastcdr from a ROS tree).
DEP_HINTS=()
for dep in fastcdr fastrtps fastdds foonathan_memory; do
  dir="$(sed -n "s/^${dep}_DIR:[A-Z]*=//p" "$IBMW_BUILD/CMakeCache.txt" | head -n1)"
  if [[ -n "$dir" && "$dir" != *-NOTFOUND ]]; then DEP_HINTS+=("-D${dep}_DIR=$dir"); fi
done

build_fixture() {  # <name> [cmake args...]
  local name="$1"; shift
  cmake -S "$FIXTURE" -B "$SCRATCH/$name-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DCMAKE_INSTALL_PREFIX="$SCRATCH/$name-prefix" "${DEP_HINTS[@]}" "$@" >/dev/null
  cmake --build "$SCRATCH/$name-build" --parallel "$JOBS" >/dev/null
  cmake --install "$SCRATCH/$name-build" >/dev/null
}

write_config() {  # <cfg> <pkg> <enable_crash_log>
  cat >"$1" <<YAML
ibmw:
  logging:
    core_level: INFO
    enable_crash_log: $3
    backends:
      - type: console
  scheduling:
    executors:
      - name: work_executor
        type: thread_pool
        options:
          thread_num: 2
  modules:
    packages:
      - path: $2
        enable_modules: [CoroFixtureModule]
    modules:
      - name: CoroFixtureModule
        log_level: INFO
YAML
}

run_fixture() {  # <name> [<root holding ibmw-prefix and <name>-prefix>]
  local name="$1"
  local root="${2:-$SCRATCH}"
  local pkg="$root/$name-prefix/lib/libcoro_fixture_pkg.so"
  local cfg="$root/$name.yaml"
  local log="$root/$name.log"
  write_config "$cfg" "$pkg" false
  local rc=0
  env -u LD_LIBRARY_PATH -u LD_PRELOAD "$root/ibmw-prefix/bin/ibmw" run "$cfg" --duration 2 --no-signal >"$log" 2>&1 || rc=$?
  if [[ $rc -ne 0 ]] || ! grep -q 'coro fixture result=500500' "$log" || ! grep -q 'IBMW stopped' "$log"; then
    cat "$log" >&2
    fail "$name: package did not run through bin/ibmw (rc=$rc)"
  fi
  note "$name: ran through bin/ibmw, coro fixture result=500500"
}

note "building fixture package with default install RPATH"
build_fixture default
PKG_DYN="$(readelf -d "$SCRATCH/default-prefix/lib/libcoro_fixture_pkg.so")"
grep -q "(NEEDED).*\[$SONAME\]" <<<"$PKG_DYN" || { printf '%s\n' "$PKG_DYN" >&2; fail "default: package has no DT_NEEDED $SONAME"; }
PKG_RUNPATH="$(sed -n 's/.*(\(RUNPATH\|RPATH\)).*\[\(.*\)\]/\2/p' <<<"$PKG_DYN")"
[[ "$PKG_RUNPATH" == '$ORIGIN:$ORIGIN/../../ibmw-prefix/lib' ]] ||
  { printf '%s\n' "$PKG_DYN" >&2; fail "default: unexpected package run path '$PKG_RUNPATH'"; }
note "default: package NEEDED $SONAME, run path $PKG_RUNPATH"
run_fixture default

# Both trees moved together: the package finds the library by its own RUNPATH
# (ldd, without bin/ibmw), and it still runs through the moved bin/ibmw.
MOVED="$SCRATCH/moved"
mkdir -p "$MOVED/ibmw-prefix" "$MOVED/default-prefix"
cp -a "$PREFIX/." "$MOVED/ibmw-prefix/"
cp -a "$SCRATCH/default-prefix/." "$MOVED/default-prefix/"
MOVED_LDD="$(env -u LD_LIBRARY_PATH -u LD_PRELOAD ldd "$MOVED/default-prefix/lib/libcoro_fixture_pkg.so")"
grep -qF "$SONAME => $MOVED/default-prefix/lib/../../ibmw-prefix/lib/$SONAME" <<<"$MOVED_LDD" ||
  { printf '%s\n' "$MOVED_LDD" >&2; fail "moved: package does not find $SONAME through its run path"; }
note "moved: package finds $SONAME through its run path"
run_fixture default "$MOVED"

note "building fixture package with CMAKE_SKIP_RPATH=ON"
build_fixture norpath -DCMAKE_SKIP_RPATH=ON
PKG_DYN="$(readelf -d "$SCRATCH/norpath-prefix/lib/libcoro_fixture_pkg.so")"
grep -q "(NEEDED).*\[$SONAME\]" <<<"$PKG_DYN" || { printf '%s\n' "$PKG_DYN" >&2; fail "norpath: package has no DT_NEEDED $SONAME"; }
if grep -q '(RUNPATH\|RPATH)' <<<"$PKG_DYN"; then
  printf '%s\n' "$PKG_DYN" >&2; fail "norpath: package unexpectedly has a run path"
fi
note "norpath: package NEEDED $SONAME, no run path"
run_fixture norpath

run_crash() {  # <mode> <crashing function>: crash in the default package's Run()
  local mode="$1"
  local cfg="$SCRATCH/crash-$mode.yaml"
  local log="$SCRATCH/crash-$mode.log"
  local pkg_name="libcoro_fixture_pkg.so"
  write_config "$cfg" "$SCRATCH/default-prefix/lib/$pkg_name" true
  local rc=0
  # No core dump for the intended crash (ulimit in a subshell only).
  (ulimit -c 0; exec env -u LD_LIBRARY_PATH -u LD_PRELOAD IBMW_CORO_FIXTURE_CRASH="$mode" \
    "$IBMW_BIN" run "$cfg" --duration 2 --no-signal) >"$log" 2>&1 || rc=$?
  local raw
  raw="$(sed -n '/^\*\*\* raw stacktrace/,/^\*\*\* symbolized stacktrace/p' "$log")"
  local problems=()
  [[ $rc -eq 139 ]] || problems+=("exit status $rc, expected 139 (SIGSEGV)")
  grep -q '^\*\*\* IBMW crash: signal 11 (SIGSEGV)' "$log" || problems+=("no header")
  grep -q "^\*\*\* pc 0x[0-9a-f]*  .*/$pkg_name+0x" "$log" || problems+=("pc line does not name $pkg_name")
  grep -q "^#00 pc 0x[0-9a-f]*  .*/$pkg_name+0x" <<<"$raw" || problems+=("raw frame #00 does not name $pkg_name")
  if grep -q '^\*\*\* caller' "$log"; then problems+=("caller line for a data fault"); fi
  if grep -q 'faulted' "$log"; then problems+=("fault inside the handler"); fi
  grep -q '^\*\*\* end of stacktrace' "$log" || problems+=("no end of stacktrace")
  grep -q "$2" "$log" || problems+=("symbolized part does not name $2")
  # The same report through the log: the callback logs it and then waits
  # (bounded, LoggingSubsystem::FlushForCrash) until the console backend,
  # which writes on the watchdog executor, has written it. The stderr report
  # comes first, so what follows the callback line is the logged copy; the
  # formatter's "(file:line)" suffix is only written after the whole message.
  local logged
  logged="$(sed -n '/Crash signal handling stacktrace/,$p' "$log")"
  if [[ -z "$logged" ]]; then
    problems+=("log callback line not written")
  else
    grep -q '^\*\*\* end of stacktrace' <<<"$logged" || problems+=("logged report has no end of stacktrace")
    grep -q "$2" <<<"$logged" || problems+=("logged report does not name $2")
    grep -q '(crash_signal_handling\.cc:[0-9]*)' <<<"$logged" || problems+=("logged report line is cut off")
  fi
  if [[ ${#problems[@]} -gt 0 ]]; then
    cat "$log" >&2
    fail "crash-$mode: ${problems[*]}"
  fi
  note "crash-$mode: full report on stderr and in the log through bin/ibmw, package frames named $pkg_name+0x..."
}

if grep -q '^IBMW_SELFDEV_CRASH_HANDLER:BOOL=ON' "$IBMW_BUILD/CMakeCache.txt"; then
  run_crash overflow CoroFixtureCrashRecurse
  run_crash null CoroFixtureCrashNullWrite
else
  note "IBMW_SELFDEV_CRASH_HANDLER is OFF: crash report checks not applicable"
fi

note "PASS"
