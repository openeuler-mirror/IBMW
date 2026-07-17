#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# prepare_ohos_ros2_rosidl_overlay.sh — prepare or validate the OHOS ARM64
# geometry_msgs rosidl overlay used by the ROS2 compile-only gate.
#
# This helper is intentionally narrow:
#   - prepares/checks only geometry_msgs as an overlay over an existing OHOS ROS2 underlay
#   - --prepare builds from a temporary sanitized copy so the original source stays untouched
#   - emits the overlay:underlay prefix list consumed by check_ohos_ros2_rosidl_compile.sh
#   - never enables rclcpp / IBMW_BUILD_WITH_ROS2
#   - never claims board/runtime validation
#   - treats IBMW_OHOS_PYTHON_EXECUTABLE as a host codegen interpreter only;
#     --prepare intentionally does not consume Python include/library target paths

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/deps/prepare_ohos_ros2_rosidl_overlay.sh (--check-only|--prepare) [--run-compile-check] [build-dir]

If build-dir is omitted, the helper uses the repo-owned default build tree
under <repo>/build-ohos-arm64-dds-source-deps. An explicit build-dir may be
outside the repository. The helper never deletes build-dir itself; it only
refreshes strict child directories carrying its ownership marker. Existing
unmarked child directories are refused, and TMPDIR grants no ownership.

Modes:
  --check-only         Validate an existing geometry_msgs overlay + ROS2 underlay.
  --prepare            Build a sanitized geometry_msgs copy into the overlay prefix, then validate it.
  --run-compile-check   After validation, invoke scripts/check_ohos_ros2_rosidl_compile.sh.

Environment variables:
  IBMW_OHOS_ROS2_UNDERLAY_PREFIX       Required. Existing OHOS ROS2 underlay prefix.
  IBMW_OHOS_ROS2_OVERLAY_PREFIX        Optional. Defaults to <build-dir>/ros2_geometry_overlay.
  IBMW_OHOS_ROS2_GEOMETRY_MSGS_SOURCE  Required for --prepare. geometry_msgs source directory.
  IBMW_OHOS_ROS2_PYTHONPATH            Required. ament/rosidl Python package path.
  IBMW_OHOS_SDK_NATIVE_ROOT            Required for --prepare and compile check.
  IBMW_OHOS_PYTHON_EXECUTABLE          Required for --prepare. Host generator interpreter only.
                                        Python include/library inputs are intentionally unused.

Typical OHOS Humble path shapes are only templates, not defaults:
  <ohos-sdk-native-root>/sysroot/usr
  <ohos-ros2-underlay-prefix>

When --run-compile-check is used, the usual check_ohos_ros2_rosidl_compile.sh
environment variables for Python/OpenSSL/OHOS paths must also be set.
EOF
}

fail() {
  echo "[ohos-rosidl-overlay] FAIL: $*" >&2
  exit 1
}

error_exit() {
  echo "[ohos-rosidl-overlay] FAIL: $*" >&2
  exit 2
}

note() {
  echo "[ohos-rosidl-overlay] $*"
}

pass() {
  echo "[ohos-rosidl-overlay] PASS: $*"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

if ! command -v realpath >/dev/null 2>&1; then
  error_exit "realpath not found on PATH"
fi

canonical_path() {
  realpath -m -- "$1"
}

REPO_ROOT="$(canonical_path "$REPO_ROOT")"

MODE=""
RUN_COMPILE_CHECK=0
BUILD_DIR=""
BUILD_DIR_EXPLICIT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --help|-h)
      usage
      exit 0
      ;;
    --check-only)
      if [[ -n "$MODE" ]]; then
        fail "choose only one mode: --check-only or --prepare"
      fi
      MODE="check"
      shift
      ;;
    --prepare)
      if [[ -n "$MODE" ]]; then
        fail "choose only one mode: --check-only or --prepare"
      fi
      MODE="prepare"
      shift
      ;;
    --run-compile-check)
      RUN_COMPILE_CHECK=1
      shift
      ;;
    --*)
      usage >&2
      fail "unknown option: $1"
      ;;
    *)
      if [[ -n "$BUILD_DIR" ]]; then
        usage >&2
        fail "unexpected extra argument: $1"
      fi
      BUILD_DIR="$1"
      BUILD_DIR_EXPLICIT=1
      shift
      ;;
  esac
done

if [[ -z "$MODE" ]]; then
  usage >&2
  fail "explicit mode required: --check-only or --prepare"
fi

BUILD_DIR_INPUT="${BUILD_DIR:-$REPO_ROOT/build-ohos-arm64-dds-source-deps}"
OVERLAY_PREFIX_INPUT="${IBMW_OHOS_ROS2_OVERLAY_PREFIX:-}"
BUILD_DIR="$(canonical_path "$BUILD_DIR_INPUT")"
UNDERLAY_PREFIX="${IBMW_OHOS_ROS2_UNDERLAY_PREFIX:-}"
if [[ -n "$UNDERLAY_PREFIX" ]]; then
  UNDERLAY_PREFIX="$(canonical_path "$UNDERLAY_PREFIX")"
fi
OVERLAY_PREFIX="$(canonical_path "${OVERLAY_PREFIX_INPUT:-$BUILD_DIR/ros2_geometry_overlay}")"
GEOMETRY_SOURCE="${IBMW_OHOS_ROS2_GEOMETRY_MSGS_SOURCE:-}"
if [[ -n "$GEOMETRY_SOURCE" ]]; then
  GEOMETRY_SOURCE="$(canonical_path "$GEOMETRY_SOURCE")"
fi
ROS2_PYTHONPATH="${IBMW_OHOS_ROS2_PYTHONPATH:-}"
SDK_NATIVE_ROOT="${IBMW_OHOS_SDK_NATIVE_ROOT:-${OHOS_SDK_NATIVE_ROOT:-}}"
if [[ -n "$SDK_NATIVE_ROOT" ]]; then
  SDK_NATIVE_ROOT="$(canonical_path "$SDK_NATIVE_ROOT")"
fi
PYTHON_EXECUTABLE="${IBMW_OHOS_PYTHON_EXECUTABLE:-}"
PYTHON_INCLUDE_DIR="${IBMW_OHOS_PYTHON_INCLUDE_DIR:-}"
PYTHON_LIBRARY="${IBMW_OHOS_PYTHON_LIBRARY:-}"
OPENSSL_ROOT_DIR="${IBMW_OHOS_OPENSSL_ROOT_DIR:-}"
OPENSSL_INCLUDE_DIR="${IBMW_OHOS_OPENSSL_INCLUDE_DIR:-}"
OPENSSL_SSL_LIBRARY="${IBMW_OHOS_OPENSSL_SSL_LIBRARY:-}"
OPENSSL_CRYPTO_LIBRARY="${IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY:-}"
OVERLAY_SOURCE_DIR="$BUILD_DIR/ros2_geometry_overlay_src/geometry_msgs"
OVERLAY_BUILD_DIR="$BUILD_DIR/ros2_geometry_overlay_build"
OVERLAY_LOG_DIR="$BUILD_DIR/ros2_geometry_overlay_log"
OVERLAY_SOURCE_ROOT="$(canonical_path "$(dirname "$OVERLAY_SOURCE_DIR")")"
PREFIX_LIST="$OVERLAY_PREFIX:$UNDERLAY_PREFIX"

require_command() {
  local command_name="$1"
  if ! command -v "$command_name" >/dev/null 2>&1; then
    error_exit "$command_name not found on PATH"
  fi
}

require_nonempty() {
  local value="$1"
  local label="$2"
  if [[ -z "$value" ]]; then
    fail "$label is required"
  fi
}

require_file() {
  local file_path="$1"
  local label="$2"
  if [[ ! -f "$file_path" ]]; then
    fail "$label not found: $file_path"
  fi
}

require_dir() {
  local dir_path="$1"
  local label="$2"
  if [[ ! -d "$dir_path" ]]; then
    fail "$label not found: $dir_path"
  fi
}

reject_forbidden_path() {
  local value="$1"
  local label="$2"
  if [[ "$value" == *'/opt/ros/humble'* ]]; then
    fail "$label must not use host ROS Humble as target evidence: $value"
  fi
  if [[ "$value" == *'/usr/lib/x86_64-linux-gnu'* ]]; then
    fail "$label must not use host x86_64 libraries as target evidence: $value"
  fi
}

path_is_under() {
  local path="$1"
  local root="$2"
  [[ "$path" == "$root" || "$path" == "$root"/* ]]
}

path_overlaps() {
  local first="$1"
  local second="$2"
  path_is_under "$first" "$second" || path_is_under "$second" "$first"
}

reject_lexical_traversal() {
  local value="$1"
  local label="$2"
  if [[ "/$value/" == *'/../'* ]]; then
    fail "$label must not contain parent-directory traversal: $value"
  fi
}

require_safe_build_root() {
  local relative top

  reject_lexical_traversal "$BUILD_DIR_INPUT" 'build directory'
  if [[ -n "$OVERLAY_PREFIX_INPUT" ]]; then
    reject_lexical_traversal "$OVERLAY_PREFIX_INPUT" 'IBMW_OHOS_ROS2_OVERLAY_PREFIX'
  fi

  case "$BUILD_DIR" in
    /|"${HOME:-}"|"$REPO_ROOT")
      fail "build directory is a protected path: $BUILD_DIR"
      ;;
  esac

  if [[ -n "$UNDERLAY_PREFIX" ]] && path_overlaps "$BUILD_DIR" "$UNDERLAY_PREFIX"; then
    fail "build directory overlaps the ROS2 underlay: $BUILD_DIR"
  fi
  if [[ -n "$GEOMETRY_SOURCE" ]] && path_overlaps "$BUILD_DIR" "$GEOMETRY_SOURCE"; then
    fail "build directory overlaps the geometry_msgs source: $BUILD_DIR"
  fi
  if [[ -n "$SDK_NATIVE_ROOT" ]] && path_overlaps "$BUILD_DIR" "$SDK_NATIVE_ROOT"; then
    fail "build directory overlaps the OHOS SDK root: $BUILD_DIR"
  fi

  if path_is_under "$BUILD_DIR" "$REPO_ROOT"; then
    relative="${BUILD_DIR#"$REPO_ROOT"/}"
    top="${relative%%/*}"
    case "$top" in
      build|build-*|build_*|cmake-build-*) ;;
      *) fail "repository-local build directory must be below a build* directory: $BUILD_DIR" ;;
    esac
  elif path_is_under "$REPO_ROOT" "$BUILD_DIR"; then
    fail "external build directory must not contain the repository root: $BUILD_DIR"
  elif [[ "$BUILD_DIR_EXPLICIT" -ne 1 ]]; then
    fail "internal error: default build directory resolved outside the repository: $BUILD_DIR"
  fi
}

require_safe_helper_path() {
  local path="$1"
  local label="$2"
  local normalized

  require_nonempty "$path" "$label"
  normalized="$(canonical_path "$path")"
  if [[ "$normalized" != "$path" ]]; then
    fail "$label must be normalized before deletion: $path -> $normalized"
  fi

  case "$normalized" in
    /|"${HOME:-}"|"$REPO_ROOT")
      fail "$label is not a helper-owned path: $path"
      ;;
  esac

  if [[ -n "$UNDERLAY_PREFIX" ]] && path_overlaps "$normalized" "$UNDERLAY_PREFIX"; then
    fail "$label overlaps the ROS2 underlay: $path"
  fi
  if [[ -n "$GEOMETRY_SOURCE" ]] && path_overlaps "$normalized" "$GEOMETRY_SOURCE"; then
    fail "$label overlaps the geometry_msgs source: $path"
  fi
  if [[ -n "$SDK_NATIVE_ROOT" ]] && path_overlaps "$normalized" "$SDK_NATIVE_ROOT"; then
    fail "$label overlaps the OHOS SDK root: $path"
  fi

  if [[ "$normalized" == "$BUILD_DIR" ]] || ! path_is_under "$normalized" "$BUILD_DIR"; then
    fail "$label must be a strict child of the selected build directory: $path"
  fi
}

require_disjoint_helper_roots() {
  local roots=(
    "$OVERLAY_BUILD_DIR"
    "$OVERLAY_LOG_DIR"
    "$OVERLAY_PREFIX"
    "$OVERLAY_SOURCE_ROOT"
  )
  local labels=(
    'overlay build directory'
    'overlay log directory'
    'ROS2 geometry overlay prefix'
    'overlay source root'
  )
  local first second

  for ((first = 0; first < ${#roots[@]}; first++)); do
    for ((second = first + 1; second < ${#roots[@]}; second++)); do
      if path_overlaps "${roots[first]}" "${roots[second]}"; then
        fail "helper-owned roots must be distinct and must not contain one another: ${labels[first]}=${roots[first]}; ${labels[second]}=${roots[second]}"
      fi
    done
  done
}

reset_helper_owned_directory() {
  local path="$1"
  local label="$2"
  local marker="$path/.ibmw_rosidl_overlay_owned"

  require_safe_helper_path "$path" "$label"
  if [[ -e "$path" || -L "$path" ]]; then
    if [[ ! -f "$marker" ]] || [[ "$(<"$marker")" != 'IBMW prepare_ohos_ros2_rosidl_overlay.sh' ]]; then
      fail "$label already exists without the helper ownership marker; refusing to delete: $path"
    fi
    rm -rf -- "$path"
  fi
  mkdir -p -- "$path"
  printf '%s\n' 'IBMW prepare_ohos_ros2_rosidl_overlay.sh' > "$marker"
}

validate_package_xml_name() {
  local package_xml="$1"
  if ! grep -Eq '<name>[[:space:]]*geometry_msgs[[:space:]]*</name>' "$package_xml"; then
    fail "package.xml does not declare geometry_msgs: $package_xml"
  fi
}

print_exports() {
  cat <<EOF
[ohos-rosidl-overlay] export IBMW_OHOS_ROS2_PREFIX_LIST="$PREFIX_LIST"
[ohos-rosidl-overlay] export IBMW_OHOS_ROS2_PYTHONPATH="$ROS2_PYTHONPATH"
EOF
}

validate_overlay_prefix() {
  require_dir "$OVERLAY_PREFIX" 'ROS2 geometry overlay prefix'
  require_dir "$UNDERLAY_PREFIX" 'ROS2 underlay prefix'
  require_dir "$ROS2_PYTHONPATH" 'ROS2 Python path'

  bash "$REPO_ROOT/scripts/deps/check_target_ros2_prefix.sh" "$PREFIX_LIST" fastdds types rosidl
}

prepare_overlay() {
  require_safe_build_root
  require_safe_helper_path "$OVERLAY_BUILD_DIR" 'overlay build directory'
  require_safe_helper_path "$OVERLAY_LOG_DIR" 'overlay log directory'
  require_safe_helper_path "$OVERLAY_PREFIX" 'ROS2 geometry overlay prefix'
  require_safe_helper_path "$OVERLAY_SOURCE_ROOT" 'overlay source root'
  require_disjoint_helper_roots

  require_command colcon
  require_nonempty "$GEOMETRY_SOURCE" 'IBMW_OHOS_ROS2_GEOMETRY_MSGS_SOURCE'
  require_nonempty "$SDK_NATIVE_ROOT" 'IBMW_OHOS_SDK_NATIVE_ROOT'
  if [[ -z "$OPENSSL_ROOT_DIR" ]]; then
    OPENSSL_ROOT_DIR="$SDK_NATIVE_ROOT/sysroot/usr"
  fi
  if [[ -z "$OPENSSL_INCLUDE_DIR" ]]; then
    OPENSSL_INCLUDE_DIR="$OPENSSL_ROOT_DIR/include"
  fi
  if [[ -z "$OPENSSL_SSL_LIBRARY" ]]; then
    OPENSSL_SSL_LIBRARY="$OPENSSL_ROOT_DIR/lib/libssl.so"
  fi
  if [[ -z "$OPENSSL_CRYPTO_LIBRARY" ]]; then
    OPENSSL_CRYPTO_LIBRARY="$OPENSSL_ROOT_DIR/lib/libcrypto.so"
  fi
  require_nonempty "$PYTHON_EXECUTABLE" 'IBMW_OHOS_PYTHON_EXECUTABLE'
  require_dir "$GEOMETRY_SOURCE" 'geometry_msgs source directory'
  require_file "$GEOMETRY_SOURCE/package.xml" 'geometry_msgs package.xml'
  validate_package_xml_name "$GEOMETRY_SOURCE/package.xml"
  require_dir "$SDK_NATIVE_ROOT" 'OHOS SDK native root'
  require_file "$SDK_NATIVE_ROOT/build/cmake/ohos.toolchain.cmake" 'OHOS SDK CMake toolchain'
  require_file "$PYTHON_EXECUTABLE" 'Python executable'
  require_dir "$OPENSSL_ROOT_DIR" 'OpenSSL root directory'
  require_dir "$OPENSSL_INCLUDE_DIR" 'OpenSSL include directory'
  require_file "$OPENSSL_SSL_LIBRARY" 'OpenSSL SSL library'
  require_file "$OPENSSL_CRYPTO_LIBRARY" 'OpenSSL crypto library'

  mkdir -p "$BUILD_DIR"

  note "refreshing helper-owned overlay build/install/log state"
  reset_helper_owned_directory "$OVERLAY_BUILD_DIR" 'overlay build directory'
  reset_helper_owned_directory "$OVERLAY_LOG_DIR" 'overlay log directory'
  reset_helper_owned_directory "$OVERLAY_PREFIX" 'ROS2 geometry overlay prefix'

  prepare_sanitized_geometry_source() {
    local source_dir="$GEOMETRY_SOURCE"
    local overlay_root
    local overlay_cmake

    overlay_root="$OVERLAY_SOURCE_ROOT"
    overlay_cmake="$OVERLAY_SOURCE_DIR/CMakeLists.txt"

    reset_helper_owned_directory "$overlay_root" 'overlay source root'
    mkdir -p "$OVERLAY_SOURCE_DIR"
    cp -a "$source_dir"/. "$OVERLAY_SOURCE_DIR"/

    if [[ ! -f "$overlay_cmake" ]]; then
      fail "sanitized geometry_msgs CMakeLists.txt not found: $overlay_cmake"
    fi

    perl -0pi -e 's/find_package\(rosidl_default_generators REQUIRED\)/find_package(rosidl_generator_c REQUIRED)\nfind_package(rosidl_generator_cpp REQUIRED)\nfind_package(rosidl_typesupport_c REQUIRED)\nfind_package(rosidl_typesupport_cpp REQUIRED)\nfind_package(rosidl_typesupport_fastrtps_c REQUIRED)\nfind_package(rosidl_typesupport_fastrtps_cpp REQUIRED)\nfind_package(rosidl_typesupport_introspection_c REQUIRED)\nfind_package(rosidl_typesupport_introspection_cpp REQUIRED)/g' "$overlay_cmake"

    if grep -q 'rosidl_default_generators' "$overlay_cmake"; then
      fail "sanitized geometry_msgs CMakeLists.txt still references rosidl_default_generators: $overlay_cmake"
    fi

    if grep -q 'rosidl_generator_py' "$overlay_cmake"; then
      fail "sanitized geometry_msgs CMakeLists.txt still references rosidl_generator_py: $overlay_cmake"
    fi
  }

  prepare_sanitized_geometry_source

  note "building geometry_msgs overlay"
  note "source:  $GEOMETRY_SOURCE"
  note "temp:    $OVERLAY_SOURCE_DIR"
  note "overlay: $OVERLAY_PREFIX"
  note "underlay: $UNDERLAY_PREFIX"

  PYTHONPATH="$ROS2_PYTHONPATH" \
  AMENT_PREFIX_PATH="$UNDERLAY_PREFIX" \
  CMAKE_PREFIX_PATH="$UNDERLAY_PREFIX" \
  PKG_CONFIG_PATH= \
  PKG_CONFIG_LIBDIR="$OPENSSL_ROOT_DIR/lib/pkgconfig" \
  colcon --log-base "$OVERLAY_LOG_DIR" build \
    --merge-install \
    --base-paths "$OVERLAY_SOURCE_DIR" \
    --packages-select geometry_msgs \
    --install-base "$OVERLAY_PREFIX" \
    --build-base "$OVERLAY_BUILD_DIR" \
    --cmake-force-configure \
    --cmake-args \
      -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchains/ohos-arm64-v8a.cmake" \
      -DOHOS_SDK_NATIVE_ROOT="$SDK_NATIVE_ROOT" \
      -DCMAKE_PREFIX_PATH="$UNDERLAY_PREFIX" \
      -DCMAKE_FIND_ROOT_PATH="$UNDERLAY_PREFIX;$SDK_NATIVE_ROOT/sysroot" \
      -Dament_cmake_DIR="$UNDERLAY_PREFIX/share/ament_cmake/cmake" \
      -DPYTHON_EXECUTABLE="$PYTHON_EXECUTABLE" \
      -DPython3_EXECUTABLE="$PYTHON_EXECUTABLE" \
      -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT_DIR" \
      -DOPENSSL_INCLUDE_DIR="$OPENSSL_INCLUDE_DIR" \
      -DOPENSSL_SSL_LIBRARY="$OPENSSL_SSL_LIBRARY" \
      -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_CRYPTO_LIBRARY" \
      -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH \
      -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE \
      -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE
}

scan_overlay_artifacts_for_leakage() {
  local scan_roots=()
  local scan_root
  local matched=0
  local python_env_snapshot_matched=0

  for scan_root in "$OVERLAY_BUILD_DIR" "$OVERLAY_LOG_DIR" "$OVERLAY_PREFIX"; do
    if [[ -d "$scan_root" ]]; then
      scan_roots+=("$scan_root")
    fi
  done

  if [[ ${#scan_roots[@]} -eq 0 ]]; then
    return 0
  fi

  for scan_root in "${scan_roots[@]}"; do
    while IFS= read -r match_line; do
      # CMake records all configure-time dependency inputs in Makefile.cmake.
      # A source-built ROS 2 underlay may contain rosidl_generator_py export
      # metadata for packages such as builtin_interfaces/std_msgs even though
      # this overlay never builds or installs Python message artifacts.
      if [[ "$match_line" == *'/CMakeFiles/Makefile.cmake:'* && \
            "$match_line" == *'/export_'*'__rosidl_generator_pyExport'*'.cmake'* ]]; then
        continue
      fi
      matched=1
      break 2
    done < <(grep -R -n -F -I -e 'geometry_msgs__python' -e 'rosidl_generator_py' -e '__pyext' -e '/usr/lib/x86_64-linux-gnu' -e '/opt/ros/humble' "$scan_root" 2>/dev/null || true)
  done

  if [[ "$matched" -eq 1 ]]; then
    fail 'overlay artifacts contain forbidden Python package or host-path leakage evidence'
  fi

  for scan_root in "${scan_roots[@]}"; do
    local python_library_basename
    python_library_basename="$(basename "${PYTHON_LIBRARY:-libpython}")"

    while IFS= read -r match_line; do
      if [[ "$match_line" == *'IBMW_OHOS_PYTHON_LIBRARY='* || "$match_line" == *"env': OrderedDict"* ]]; then
        python_env_snapshot_matched=1
        continue
      fi
      matched=1
      break 2
    done < <(grep -R -n -F -I -e "$python_library_basename" "$scan_root" 2>/dev/null || true)
  done

  if [[ "$matched" -eq 1 ]]; then
    fail 'overlay artifacts contain forbidden Python library linkage evidence'
  fi

  if [[ "$python_env_snapshot_matched" -eq 1 ]]; then
    note 'ignoring libpython3.13.so only captured in IBMW_OHOS_PYTHON_LIBRARY env snapshots'
  fi
}

run_compile_check() {
  IBMW_OHOS_ROS2_PREFIX_LIST="$PREFIX_LIST" \
  IBMW_OHOS_ROS2_PYTHONPATH="$ROS2_PYTHONPATH" \
  bash "$REPO_ROOT/scripts/check_ohos_ros2_rosidl_compile.sh" "$BUILD_DIR"
}

require_command bash
require_nonempty "$UNDERLAY_PREFIX" 'IBMW_OHOS_ROS2_UNDERLAY_PREFIX'
require_nonempty "$ROS2_PYTHONPATH" 'IBMW_OHOS_ROS2_PYTHONPATH'
reject_forbidden_path "$UNDERLAY_PREFIX" 'IBMW_OHOS_ROS2_UNDERLAY_PREFIX'
reject_forbidden_path "$OVERLAY_PREFIX" 'IBMW_OHOS_ROS2_OVERLAY_PREFIX'
reject_forbidden_path "$ROS2_PYTHONPATH" 'IBMW_OHOS_ROS2_PYTHONPATH'

case "$MODE" in
  prepare)
    prepare_overlay
    scan_overlay_artifacts_for_leakage
    ;;
  check)
    scan_overlay_artifacts_for_leakage
    ;;
  *)
    fail "internal error: unsupported mode $MODE"
    ;;
esac

validate_overlay_prefix
print_exports

if [[ "$RUN_COMPILE_CHECK" -eq 1 ]]; then
  run_compile_check
fi

pass 'OHOS geometry_msgs rosidl overlay validated.'
