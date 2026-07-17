#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Synthetic deletion-boundary tests for prepare_ohos_ros2_rosidl_overlay.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
HELPER="$REPO_ROOT/scripts/deps/prepare_ohos_ros2_rosidl_overlay.sh"
SCRATCH="$REPO_ROOT/build-rosidl-path-test-${UID:-user}-$$-$RANDOM"
if [[ -e "$SCRATCH" ]]; then
  echo "FAIL: unique scratch path already exists: $SCRATCH" >&2
  exit 1
fi
mkdir -p "$SCRATCH"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $1" >&2
  [[ $# -lt 2 ]] || printf '%s\n' "$2" >&2
  exit 1
}

run_expect_fail() {
  local label="$1" build_dir="$2" overlay="$3" tmpdir="$4"
  local out status
  set +e
  out="$(PATH="$FAKE_BIN:$PATH" TMPDIR="$tmpdir" \
    IBMW_OHOS_ROS2_UNDERLAY_PREFIX="$UNDERLAY" \
    IBMW_OHOS_ROS2_OVERLAY_PREFIX="$overlay" \
    IBMW_OHOS_ROS2_GEOMETRY_MSGS_SOURCE="$SOURCE" \
    IBMW_OHOS_ROS2_PYTHONPATH="$PYTHONPATH_DIR" \
    IBMW_OHOS_SDK_NATIVE_ROOT="$SDK" \
    IBMW_OHOS_PYTHON_EXECUTABLE="$PYTHON" \
    bash "$HELPER" --prepare "$build_dir" 2>&1)"
  status=$?
  set -e
  [[ $status -ne 0 ]] || fail "$label unexpectedly passed" "$out"
  printf '%s\n' "$out"
}

OWNERSHIP_MARKER='.ibmw_rosidl_overlay_owned'
OWNERSHIP_VALUE='IBMW prepare_ohos_ros2_rosidl_overlay.sh'

mark_helper_owned() {
  local path="$1"
  mkdir -p "$path"
  printf '%s\n' "$OWNERSHIP_VALUE" > "$path/$OWNERSHIP_MARKER"
}

UNDERLAY="$SCRATCH/underlay"
SOURCE="$SCRATCH/geometry_msgs"
PYTHONPATH_DIR="$SCRATCH/pythonpath"
SDK="$SCRATCH/sdk"
PYTHON="$SCRATCH/python"
FAKE_BIN="$SCRATCH/bin"
mkdir -p "$UNDERLAY" "$SOURCE" "$PYTHONPATH_DIR" "$SDK/build/cmake" \
  "$SDK/sysroot/usr/include" "$SDK/sysroot/usr/lib" "$FAKE_BIN"
printf '<package><name>geometry_msgs</name></package>\n' > "$SOURCE/package.xml"
printf '# synthetic\n' > "$SOURCE/CMakeLists.txt"
printf '# synthetic\n' > "$SDK/build/cmake/ohos.toolchain.cmake"
printf 'synthetic\n' > "$SDK/sysroot/usr/lib/libssl.so"
printf 'synthetic\n' > "$SDK/sysroot/usr/lib/libcrypto.so"
printf '#!/usr/bin/env sh\n' > "$PYTHON"
chmod +x "$PYTHON"
cat > "$FAKE_BIN/colcon" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
install_base=''
while [[ $# -gt 0 ]]; do
  if [[ "$1" == '--install-base' ]]; then
    install_base="$2"
    break
  fi
  shift
done
[[ -n "$install_base" ]]
mkdir -p "$install_base/share/geometry_msgs"
printf '<package><name>geometry_msgs</name></package>\n' > "$install_base/share/geometry_msgs/package.xml"
EOF
chmod +x "$FAKE_BIN/colcon"

make_pkg_xml() {
  local pkg="$1"
  mkdir -p "$UNDERLAY/share/$pkg"
  printf '<package><name>%s</name></package>\n' "$pkg" > "$UNDERLAY/share/$pkg/package.xml"
}

for pkg in std_msgs ament_cmake rosidl_default_generators builtin_interfaces; do
  make_pkg_xml "$pkg"
done
for pkg in fastcdr fastdds; do
  mkdir -p "$UNDERLAY/lib/cmake/$pkg" "$UNDERLAY/lib"
  printf '# synthetic\n' > "$UNDERLAY/lib/cmake/$pkg/${pkg}Config.cmake"
  printf 'synthetic\n' > "$UNDERLAY/lib/lib${pkg}.so"
done

# A pre-existing ordinary directory must never become deletable merely because
# TMPDIR is /tmp.
ordinary_tmp="$SCRATCH/ordinary-existing"
mkdir -p "$ordinary_tmp"
printf 'keep\n' > "$ordinary_tmp/sentinel"
build_dir="$SCRATCH/repository-build"
OUT="$(run_expect_fail 'ordinary overlay with TMPDIR=/tmp' "$build_dir" "$ordinary_tmp" /tmp)"
[[ -f "$ordinary_tmp/sentinel" ]] || fail 'ordinary directory was deleted' "$OUT"

# Lexical traversal is canonicalized and cannot escape the selected build root.
mkdir -p "$build_dir"
escaped="$SCRATCH/escaped-overlay-test"
mkdir -p "$escaped"
printf 'keep\n' > "$escaped/sentinel"
OUT="$(run_expect_fail '../ overlay escape' "$build_dir" "$build_dir/../escaped-overlay-test" /tmp)"
[[ -f "$escaped/sentinel" ]] || fail '../ escape target was deleted' "$OUT"

# A custom TMPDIR is not an allowed recursive-deletion root.
custom_tmp="$SCRATCH/custom-tmp"
custom_target="$custom_tmp/existing"
mkdir -p "$custom_target"
printf 'keep\n' > "$custom_target/sentinel"
OUT="$(run_expect_fail 'custom TMPDIR overlay' "$build_dir" "$custom_target" "$custom_tmp")"
[[ -f "$custom_target/sentinel" ]] || fail 'custom TMPDIR target was deleted' "$OUT"

# Dangerous overlay prefixes are rejected, including protected roots.
for dangerous in / "$HOME" "$REPO_ROOT" "$SOURCE" "$UNDERLAY" "$SDK"; do
  OUT="$(run_expect_fail "dangerous overlay $dangerous" "$build_dir" "$dangerous" /tmp)"
done

# Even at a valid dedicated path, caller-created state without the ownership
# marker must not be recursively removed.
caller_overlay="$build_dir/caller-overlay"
mkdir -p "$caller_overlay"
printf 'keep\n' > "$caller_overlay/sentinel"
OUT="$(run_expect_fail 'unowned repository build child' "$build_dir" "$caller_overlay" /tmp)"
[[ -f "$caller_overlay/sentinel" ]] || fail 'unowned build child was deleted' "$OUT"

# An explicit external build directory is supported, but an ordinary
# pre-existing child below it is not deletable without the ownership marker.
external_unowned_build="$SCRATCH/external-unowned-build"
external_unowned_overlay="$external_unowned_build/existing-overlay"
mkdir -p "$external_unowned_overlay"
printf 'keep\n' > "$external_unowned_overlay/sentinel"
OUT="$(run_expect_fail 'unowned external build child' "$external_unowned_build" "$external_unowned_overlay" /tmp)"
[[ -f "$external_unowned_overlay/sentinel" ]] \
  || fail 'unowned external build child was deleted' "$OUT"

# All four helper-owned deletion roots must be pairwise disjoint before the
# first marked parent can be removed. Cover both path-overlap comparison
# directions by nesting the prefix below build/log roots (listed before it)
# and below the source root (listed after it).
for parent_kind in build log source; do
  overlap_build="$SCRATCH/overlap-$parent_kind-build"
  case "$parent_kind" in
    build) parent_root="$overlap_build/ros2_geometry_overlay_build" ;;
    log) parent_root="$overlap_build/ros2_geometry_overlay_log" ;;
    source) parent_root="$overlap_build/ros2_geometry_overlay_src" ;;
  esac
  mark_helper_owned "$parent_root"
  printf 'keep\n' > "$parent_root/unmarked-sentinel"
  nested_overlay="$parent_root/nested-overlay"
  OUT="$(run_expect_fail "overlay nested below marked $parent_kind root" \
    "$overlap_build" "$nested_overlay" /tmp)"
  [[ -f "$parent_root/unmarked-sentinel" ]] \
    || fail "marked $parent_kind parent was deleted before overlap rejection" "$OUT"
done

# The normal default layout and an explicit overlay below an explicit
# repository build directory remain usable.
rm -rf "$build_dir"
COMMON_ENV=(
  "PATH=$FAKE_BIN:$PATH"
  "IBMW_OHOS_ROS2_UNDERLAY_PREFIX=$UNDERLAY"
  "IBMW_OHOS_ROS2_GEOMETRY_MSGS_SOURCE=$SOURCE"
  "IBMW_OHOS_ROS2_PYTHONPATH=$PYTHONPATH_DIR"
  "IBMW_OHOS_SDK_NATIVE_ROOT=$SDK"
  "IBMW_OHOS_PYTHON_EXECUTABLE=$PYTHON"
)
OUT="$(env "${COMMON_ENV[@]}" bash "$HELPER" --prepare "$build_dir" 2>&1)" \
  || fail 'default overlay layout failed' "$OUT"
explicit_overlay="$build_dir/explicit-geometry-overlay"
OUT="$(env "${COMMON_ENV[@]}" \
  IBMW_OHOS_ROS2_OVERLAY_PREFIX="$explicit_overlay" \
  bash "$HELPER" --prepare "$build_dir" 2>&1)" \
  || fail 'explicit overlay layout failed' "$OUT"
[[ -f "$explicit_overlay/share/geometry_msgs/package.xml" ]] \
  || fail 'explicit overlay was not prepared' "$OUT"

# A caller-selected external build root remains intact while the helper creates
# and refreshes only its marked strict children.
external_build="$SCRATCH/external-build"
mkdir -p "$external_build"
printf 'keep\n' > "$external_build/build-root-sentinel"
OUT="$(env "${COMMON_ENV[@]}" bash "$HELPER" --prepare "$external_build" 2>&1)" \
  || fail 'external build directory failed' "$OUT"
OUT="$(env "${COMMON_ENV[@]}" bash "$HELPER" --prepare "$external_build" 2>&1)" \
  || fail 'external build directory refresh failed' "$OUT"
[[ -f "$external_build/build-root-sentinel" ]] \
  || fail 'external build directory itself was deleted' "$OUT"
[[ -f "$external_build/ros2_geometry_overlay/share/geometry_msgs/package.xml" ]] \
  || fail 'external build overlay was not prepared' "$OUT"

echo 'PASS: rosidl overlay helper supports explicit external build roots while rejecting TMPDIR, traversal, protected roots, overlaps, and unowned deletion targets.'
