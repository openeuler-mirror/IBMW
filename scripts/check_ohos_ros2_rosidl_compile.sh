#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# check_ohos_ros2_rosidl_compile.sh - validate the OHOS ARM64 ROS2 rosidl
# compile-only slice.
#
# This script intentionally stays narrow:
#   - validates an explicit ROS2 target overlay/underlay prefix list
#   - configures the cross-ohos-arm64-dds-source-deps preset for rosidl type support
#     and the private DDS-only Action Fibonacci compile/link gate
#   - builds only the validated compile/link targets
#   - verifies the rosidl wrapper links the ROS2-prefix FastCDR ABI
#   - rejects obvious host ROS / host target-library leakage
#   - runs the Route B target artifact audit and writes
#     <build-dir>/logs/route_b_target_artifact_audit.json
#
# Usage:
#   bash scripts/check_ohos_ros2_rosidl_compile.sh [build-dir]
#
# Required/overrideable environment variables:
#   IBMW_OHOS_ROS2_PREFIX_LIST
#   IBMW_OHOS_ROS2_PYTHONPATH
#   IBMW_OHOS_SDK_NATIVE_ROOT
#   IBMW_OHOS_PYTHON_EXECUTABLE
#   IBMW_OHOS_PYTHON_INCLUDE_DIR
#   IBMW_OHOS_PYTHON_LIBRARY
#   IBMW_OHOS_OPENSSL_ROOT_DIR
#   IBMW_OHOS_OPENSSL_INCLUDE_DIR
#   IBMW_OHOS_OPENSSL_SSL_LIBRARY
#   IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY
#   IBMW_THIRD_PARTY_LOCAL_SOURCE_ROOT Optional root containing _deps/*-src checkouts
#   IBMW_ASIO_LOCAL_SOURCE           Optional local Asio source checkout
#   IBMW_BACKWARD_LOCAL_SOURCE       Optional local backward-cpp source checkout
#   IBMW_FMT_LOCAL_SOURCE            Optional local fmt source checkout
#   IBMW_GFLAGS_LOCAL_SOURCE         Optional local gflags source checkout
#   IBMW_LIBUNIFEX_LOCAL_SOURCE       Optional local libunifex source checkout
#   IBMW_NLOHMANN_JSON_LOCAL_SOURCE  Optional local nlohmann_json source checkout
#   IBMW_TBB_LOCAL_SOURCE            Optional local oneTBB source checkout
#   IBMW_YAML_CPP_LOCAL_SOURCE       Optional local yaml-cpp source checkout

set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  bash scripts/check_ohos_ros2_rosidl_compile.sh [build-dir]

If build-dir is omitted, the helper uses the repo-owned default build tree
under <repo>/build-ohos-arm64-dds-source-deps.

Validates the OHOS ARM64 ROS2 rosidl compile-only slice. This is not board
runtime validation and does not use rclcpp/rclpy/rcl. The script separates host
generator inputs from target ROS2/DDS prefixes and finishes by running the Route
B target artifact audit.

To prepare or validate a repo-owned geometry_msgs overlay prefix first, use:
  bash scripts/deps/prepare_ohos_ros2_rosidl_overlay.sh --check-only
  bash scripts/deps/prepare_ohos_ros2_rosidl_overlay.sh --prepare --run-compile-check

Environment variables:
  IBMW_OHOS_ROS2_PREFIX_LIST
      Target ROS2 prefix list, separated by ':' or ';'. Example:
      <build-dir>/ros2_geometry_overlay:<ohos-ros2-underlay-prefix>
  IBMW_OHOS_ROS2_PYTHONPATH
      Python module path for the target ROS2 underlay's ament packages.
  IBMW_OHOS_SDK_NATIVE_ROOT
      OpenHarmony SDK native root.
  IBMW_OHOS_PYTHON_EXECUTABLE
  IBMW_OHOS_PYTHON_INCLUDE_DIR
  IBMW_OHOS_PYTHON_LIBRARY
  IBMW_OHOS_OPENSSL_ROOT_DIR
  IBMW_OHOS_OPENSSL_INCLUDE_DIR
  IBMW_OHOS_OPENSSL_SSL_LIBRARY
  IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY
  IBMW_THIRD_PARTY_LOCAL_SOURCE_ROOT
      Optional root containing cached FetchContent source checkouts named
      asio-src, backward-src, fmt-src, gflags-src, libunifex-src,
      nlohmann_json-src, tbb-src, and yaml-cpp-src.
  IBMW_ASIO_LOCAL_SOURCE
      Optional local Asio source checkout passed to CMake as
      -Dasio_LOCAL_SOURCE=... to avoid network FetchContent downloads.
  IBMW_BACKWARD_LOCAL_SOURCE
      Optional local backward-cpp source checkout.
  IBMW_FMT_LOCAL_SOURCE
      Optional local fmt source checkout passed to CMake as
      -Dfmt_LOCAL_SOURCE=... to avoid network FetchContent downloads.
  IBMW_GFLAGS_LOCAL_SOURCE
      Optional local gflags source checkout.
  IBMW_LIBUNIFEX_LOCAL_SOURCE
      Optional local libunifex source checkout passed to CMake as
      -Dlibunifex_LOCAL_SOURCE=... to avoid network FetchContent downloads.
  IBMW_NLOHMANN_JSON_LOCAL_SOURCE
      Optional local nlohmann_json source checkout.
  IBMW_TBB_LOCAL_SOURCE
      Optional local oneTBB source checkout.
  IBMW_YAML_CPP_LOCAL_SOURCE
      Optional local yaml-cpp source checkout.

The compile step consumes the overlay helper's prefix-list and Python-path
handoff. Example path shapes are templates only:
  <build-dir>/ros2_geometry_overlay:<ohos-ros2-underlay-prefix>
  <ohos-sdk-native-root>/sysroot/usr

Offline/reproducible Route B recipe shape:
  IBMW_OHOS_ROS2_PREFIX_LIST=<overlay-prefix>:<ohos-ros2-underlay-prefix> \
  IBMW_OHOS_ROS2_PYTHONPATH=<ohos-ros2-underlay-prefix>/lib/python3.12/site-packages \
  IBMW_OHOS_SDK_NATIVE_ROOT=<ohos-sdk-native-root> \
  IBMW_OHOS_PYTHON_EXECUTABLE=<host-python> \
  IBMW_OHOS_PYTHON_INCLUDE_DIR=<host-python-include-dir> \
  IBMW_OHOS_PYTHON_LIBRARY=<host-python-library> \
  IBMW_OHOS_OPENSSL_ROOT_DIR=<ohos-sdk-native-root>/sysroot/usr \
  IBMW_OHOS_OPENSSL_INCLUDE_DIR=<ohos-sdk-native-root>/sysroot/usr/include \
  IBMW_OHOS_OPENSSL_SSL_LIBRARY=<ohos-sdk-native-root>/sysroot/usr/lib/libssl.so \
  IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY=<ohos-sdk-native-root>/sysroot/usr/lib/libcrypto.so \
  IBMW_THIRD_PARTY_LOCAL_SOURCE_ROOT=<cached-fetchcontent-root> \
  bash scripts/check_ohos_ros2_rosidl_compile.sh build-ohos-arm64-dds-source-deps

PASS evidence includes:
  <build-dir>/logs/route_b_target_artifact_audit.json
with status=pass, expected_arch=aarch64, and at least one audited target ELF.
EOF
}

fail() {
  echo "[ohos-rosidl-compile] FAIL: $*" >&2
  exit 1
}

error_exit() {
  echo "[ohos-rosidl-compile] FAIL: $*" >&2
  exit 2
}

note() {
  echo "[ohos-rosidl-compile] $*"
}

pass() {
  echo "[ohos-rosidl-compile] PASS: $*"
}

if [[ $# -gt 1 ]]; then
  usage >&2
  exit 64
fi

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PRESET_NAME="cross-ohos-arm64-dds-source-deps"
DEFAULT_BUILD_DIR="$REPO_ROOT/build-ohos-arm64-dds-source-deps"
BUILD_DIR="${1:-$DEFAULT_BUILD_DIR}"
CONFIGURE_LOG="$BUILD_DIR/ohos_rosidl_configure.log"
BUILD_LOG="$BUILD_DIR/ohos_rosidl_build.log"
CACHE_FILE="$BUILD_DIR/CMakeCache.txt"
LINK_FILE="$BUILD_DIR/src/schema/ros2/ibmw_msgs/CMakeFiles/ibmw_schema_ibmw_msgs_ts.dir/link.txt"
ROUTE_B_LINK_BOUNDARY_CHECKER="$REPO_ROOT/scripts/check_ohos_route_b_link_boundary.sh"
FASTRTPS_C_LINK_FILE="$BUILD_DIR/src/schema/ros2/ibmw_msgs/CMakeFiles/ibmw_msgs__rosidl_typesupport_fastrtps_c.dir/link.txt"
FASTRTPS_CPP_LINK_FILE="$BUILD_DIR/src/schema/ros2/ibmw_msgs/CMakeFiles/ibmw_msgs__rosidl_typesupport_fastrtps_cpp.dir/link.txt"
FASTRTPS_C_FLAGS_FILE="$BUILD_DIR/src/schema/ros2/ibmw_msgs/CMakeFiles/ibmw_msgs__rosidl_typesupport_fastrtps_c.dir/flags.make"

PREFIX_LIST="${IBMW_OHOS_ROS2_PREFIX_LIST:-}"
ROS2_PYTHONPATH="${IBMW_OHOS_ROS2_PYTHONPATH:-}"
SDK_NATIVE_ROOT="${IBMW_OHOS_SDK_NATIVE_ROOT:-${OHOS_SDK_NATIVE_ROOT:-}}"
PYTHON_EXECUTABLE="${IBMW_OHOS_PYTHON_EXECUTABLE:-}"
PYTHON_INCLUDE_DIR="${IBMW_OHOS_PYTHON_INCLUDE_DIR:-}"
PYTHON_LIBRARY="${IBMW_OHOS_PYTHON_LIBRARY:-}"
OPENSSL_ROOT_DIR="${IBMW_OHOS_OPENSSL_ROOT_DIR:-}"
OPENSSL_INCLUDE_DIR="${IBMW_OHOS_OPENSSL_INCLUDE_DIR:-}"
OPENSSL_SSL_LIBRARY="${IBMW_OHOS_OPENSSL_SSL_LIBRARY:-}"
OPENSSL_CRYPTO_LIBRARY="${IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY:-}"
THIRD_PARTY_LOCAL_SOURCE_ROOT="${IBMW_THIRD_PARTY_LOCAL_SOURCE_ROOT:-}"
ASIO_LOCAL_SOURCE="${IBMW_ASIO_LOCAL_SOURCE:-}"
BACKWARD_LOCAL_SOURCE="${IBMW_BACKWARD_LOCAL_SOURCE:-}"
FMT_LOCAL_SOURCE="${IBMW_FMT_LOCAL_SOURCE:-}"
GFLAGS_LOCAL_SOURCE="${IBMW_GFLAGS_LOCAL_SOURCE:-}"
LIBUNIFEX_LOCAL_SOURCE="${IBMW_LIBUNIFEX_LOCAL_SOURCE:-}"
NLOHMANN_JSON_LOCAL_SOURCE="${IBMW_NLOHMANN_JSON_LOCAL_SOURCE:-}"
TBB_LOCAL_SOURCE="${IBMW_TBB_LOCAL_SOURCE:-}"
YAML_CPP_LOCAL_SOURCE="${IBMW_YAML_CPP_LOCAL_SOURCE:-}"

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

require_file_contains() {
  local file_path="$1"
  local expected="$2"
  local label="$3"

  require_file "$file_path" "$label file"
  if ! grep -Fq "$expected" "$file_path"; then
    fail "expected $label entry '$expected' not found in $file_path"
  fi
}

require_file_not_contains() {
  local file_path="$1"
  local forbidden="$2"
  local label="$3"

  require_file "$file_path" "$label file"
  if grep -Fq "$forbidden" "$file_path"; then
    fail "forbidden $label entry '$forbidden' found in $file_path"
  fi
}

prefix_list_to_cmake_list() {
  local raw_list="$1"
  printf '%s' "${raw_list//:/;}"
}

prefix_list_to_colon_list() {
  local raw_list="$1"
  printf '%s' "${raw_list//;/:}"
}

first_prefix_entry() {
  local raw_list="$1"
  local colon_list
  colon_list="$(prefix_list_to_colon_list "$raw_list")"
  printf '%s' "${colon_list%%:*}"
}

validate_prefix_entries() {
  local raw_list="$1"
  local colon_list entry

  colon_list="$(prefix_list_to_colon_list "$raw_list")"
  IFS=':' read -r -a _prefix_entries <<<"$colon_list"
  for entry in "${_prefix_entries[@]}"; do
    if [[ -z "$entry" ]]; then
      fail "ROS2 prefix list contains an empty entry: $raw_list"
    fi
    require_dir "$entry" "ROS2 target prefix"
  done
}

check_forbidden_value() {
  local value="$1"
  local forbidden="$2"
  local label="$3"
  if [[ "$value" == *"$forbidden"* ]]; then
    fail "$label must not contain forbidden target path '$forbidden': $value"
  fi
}

is_allowed_host_tool_line() {
  local line="$1"
  if [[ "$line" == *CMAKE_COMMAND* ||
        "$line" == *CMAKE_MAKE_PROGRAM* ||
        "$line" == *Python*_EXECUTABLE* ||
        "$line" == *PYTHON*_EXECUTABLE* ||
        "$line" == *PYTHON_EXECUTABLE* ||
        "$line" == *python* ||
        "$line" == *interpreter* ||
        "$line" == *_PROGRAM* ||
        "$line" == *" program "* ||
        "$line" == *" Program "* ||
        "$line" == *executable* ||
        "$line" == *Executable* ]]; then
    return 0
  fi
  return 1
}

check_forbidden_path_leakage() {
  local forbidden_path="$1"
  shift
  local artifact line

  for artifact in "$@"; do
    [[ -f "$artifact" ]] || continue
    while IFS= read -r line; do
      if is_allowed_host_tool_line "$line"; then
        continue
      fi
      fail "forbidden target package path leaked into validation artifacts: $line"
    done < <(grep -Fn "$forbidden_path" "$artifact" || true)
  done
}

extract_fastcdr_include_order() {
  local artifact="$1"
  local ros2_include="$2"
  local source_include="$3"
  local content
  local ros2_prefix=""
  local source_prefix=""

  [[ -f "$artifact" ]] || return 1
  content="$(tr ' ' '\n' <"$artifact")"

  if grep -Fq "$ros2_include" <<<"$content"; then
    ros2_prefix="$(grep -F -n -m 1 "$ros2_include" <<<"$content" | cut -d: -f1)"
  fi
  if grep -Fq "$source_include" <<<"$content"; then
    source_prefix="$(grep -F -n -m 1 "$source_include" <<<"$content" | cut -d: -f1)"
  fi

  if [[ -z "$ros2_prefix" ]]; then
    return 1
  fi
  if [[ -n "$source_prefix" && "$source_prefix" -lt "$ros2_prefix" ]]; then
    fail "source-prefix FastCDR include appears before ROS2-prefix include in $artifact"
  fi

  return 0
}

require_fastrtps_c_include_provenance() {
  local ros2_include_pkg="$1"
  local source_include_pkg="$2"

  if extract_fastcdr_include_order "$FASTRTPS_C_FLAGS_FILE" "$ros2_include_pkg" "$source_include_pkg"; then
    return
  fi
  if extract_fastcdr_include_order "$BUILD_DIR/compile_commands.json" "$ros2_include_pkg" "$source_include_pkg"; then
    return
  fi

  fail "ROS2-prefix FastCDR include '$ros2_include_pkg' not found for generated FastRTPS C target"
}

require_cache_entry() {
  local expected="$1"
  require_file_contains "$CACHE_FILE" "$expected" 'cache'
}

require_cache_value() {
  local cache_name="$1"
  local expected_value="$2"

  require_file "$CACHE_FILE" 'cache file'
  if ! grep -Eq "^${cache_name}:[A-Za-z_]+=${expected_value}$" "$CACHE_FILE"; then
    fail "expected ${cache_name} to be ${expected_value} in $CACHE_FILE"
  fi
}

cache_value() {
  local cache_name="$1"
  local cache_line
  cache_line="$(grep -E -m 1 "^${cache_name}(:[A-Za-z_]+)?=" "$CACHE_FILE" || true)"
  printf '%s' "${cache_line#*=}"
}

require_command cmake
require_command bash

require_nonempty "$PREFIX_LIST" 'IBMW_OHOS_ROS2_PREFIX_LIST'
require_nonempty "$ROS2_PYTHONPATH" 'IBMW_OHOS_ROS2_PYTHONPATH'
require_nonempty "$SDK_NATIVE_ROOT" 'IBMW_OHOS_SDK_NATIVE_ROOT'
require_nonempty "$PYTHON_EXECUTABLE" 'IBMW_OHOS_PYTHON_EXECUTABLE'
require_nonempty "$PYTHON_INCLUDE_DIR" 'IBMW_OHOS_PYTHON_INCLUDE_DIR'
require_nonempty "$PYTHON_LIBRARY" 'IBMW_OHOS_PYTHON_LIBRARY'
require_nonempty "$OPENSSL_ROOT_DIR" 'IBMW_OHOS_OPENSSL_ROOT_DIR'
require_nonempty "$OPENSSL_INCLUDE_DIR" 'IBMW_OHOS_OPENSSL_INCLUDE_DIR'
if [[ -z "$OPENSSL_SSL_LIBRARY" && -n "$OPENSSL_ROOT_DIR" ]]; then
  OPENSSL_SSL_LIBRARY="$OPENSSL_ROOT_DIR/lib/libssl.so"
fi
if [[ -z "$OPENSSL_CRYPTO_LIBRARY" && -n "$OPENSSL_ROOT_DIR" ]]; then
  OPENSSL_CRYPTO_LIBRARY="$OPENSSL_ROOT_DIR/lib/libcrypto.so"
fi
require_nonempty "$OPENSSL_SSL_LIBRARY" 'IBMW_OHOS_OPENSSL_SSL_LIBRARY'
require_nonempty "$OPENSSL_CRYPTO_LIBRARY" 'IBMW_OHOS_OPENSSL_CRYPTO_LIBRARY'

check_forbidden_value "$PREFIX_LIST" '/opt/ros/humble' 'IBMW_OHOS_ROS2_PREFIX_LIST'
check_forbidden_value "$PREFIX_LIST" '/usr/lib/x86_64-linux-gnu' 'IBMW_OHOS_ROS2_PREFIX_LIST'
validate_prefix_entries "$PREFIX_LIST"

require_dir "$ROS2_PYTHONPATH" 'ROS2 Python path'
require_dir "$SDK_NATIVE_ROOT" 'OHOS SDK native root'
require_file "$SDK_NATIVE_ROOT/build/cmake/ohos.toolchain.cmake" 'OHOS SDK CMake toolchain'
require_file "$PYTHON_EXECUTABLE" 'Python executable'
require_dir "$PYTHON_INCLUDE_DIR" 'Python include directory'
require_file "$PYTHON_LIBRARY" 'Python library'
require_dir "$OPENSSL_ROOT_DIR" 'OpenSSL root directory'
require_dir "$OPENSSL_INCLUDE_DIR" 'OpenSSL include directory'
require_file "$OPENSSL_SSL_LIBRARY" 'OpenSSL SSL library'
require_file "$OPENSSL_CRYPTO_LIBRARY" 'OpenSSL crypto library'
declare -a CMAKE_EXTRA_ARGS=()
if [[ -n "$THIRD_PARTY_LOCAL_SOURCE_ROOT" ]]; then
  require_dir "$THIRD_PARTY_LOCAL_SOURCE_ROOT" 'third-party local source root'
  note "third-party local source root: $THIRD_PARTY_LOCAL_SOURCE_ROOT"
  [[ -n "$ASIO_LOCAL_SOURCE" || ! -d "$THIRD_PARTY_LOCAL_SOURCE_ROOT/asio-src" ]] || ASIO_LOCAL_SOURCE="$THIRD_PARTY_LOCAL_SOURCE_ROOT/asio-src"
  [[ -n "$BACKWARD_LOCAL_SOURCE" || ! -d "$THIRD_PARTY_LOCAL_SOURCE_ROOT/backward-src" ]] || BACKWARD_LOCAL_SOURCE="$THIRD_PARTY_LOCAL_SOURCE_ROOT/backward-src"
  [[ -n "$FMT_LOCAL_SOURCE" || ! -d "$THIRD_PARTY_LOCAL_SOURCE_ROOT/fmt-src" ]] || FMT_LOCAL_SOURCE="$THIRD_PARTY_LOCAL_SOURCE_ROOT/fmt-src"
  [[ -n "$GFLAGS_LOCAL_SOURCE" || ! -d "$THIRD_PARTY_LOCAL_SOURCE_ROOT/gflags-src" ]] || GFLAGS_LOCAL_SOURCE="$THIRD_PARTY_LOCAL_SOURCE_ROOT/gflags-src"
  [[ -n "$LIBUNIFEX_LOCAL_SOURCE" || ! -d "$THIRD_PARTY_LOCAL_SOURCE_ROOT/libunifex-src" ]] || LIBUNIFEX_LOCAL_SOURCE="$THIRD_PARTY_LOCAL_SOURCE_ROOT/libunifex-src"
  [[ -n "$NLOHMANN_JSON_LOCAL_SOURCE" || ! -d "$THIRD_PARTY_LOCAL_SOURCE_ROOT/nlohmann_json-src" ]] || NLOHMANN_JSON_LOCAL_SOURCE="$THIRD_PARTY_LOCAL_SOURCE_ROOT/nlohmann_json-src"
  [[ -n "$TBB_LOCAL_SOURCE" || ! -d "$THIRD_PARTY_LOCAL_SOURCE_ROOT/tbb-src" ]] || TBB_LOCAL_SOURCE="$THIRD_PARTY_LOCAL_SOURCE_ROOT/tbb-src"
  [[ -n "$YAML_CPP_LOCAL_SOURCE" || ! -d "$THIRD_PARTY_LOCAL_SOURCE_ROOT/yaml-cpp-src" ]] || YAML_CPP_LOCAL_SOURCE="$THIRD_PARTY_LOCAL_SOURCE_ROOT/yaml-cpp-src"
fi
if [[ -n "$ASIO_LOCAL_SOURCE" ]]; then
  require_dir "$ASIO_LOCAL_SOURCE" 'Asio local source'
  note "Asio local source: $ASIO_LOCAL_SOURCE"
  CMAKE_EXTRA_ARGS+=("-Dasio_LOCAL_SOURCE=$ASIO_LOCAL_SOURCE")
fi
if [[ -n "$BACKWARD_LOCAL_SOURCE" ]]; then
  require_dir "$BACKWARD_LOCAL_SOURCE" 'backward-cpp local source'
  note "backward-cpp local source: $BACKWARD_LOCAL_SOURCE"
  CMAKE_EXTRA_ARGS+=("-Dbackward_LOCAL_SOURCE=$BACKWARD_LOCAL_SOURCE")
fi
if [[ -n "$FMT_LOCAL_SOURCE" ]]; then
  require_dir "$FMT_LOCAL_SOURCE" 'fmt local source'
  note "fmt local source: $FMT_LOCAL_SOURCE"
  CMAKE_EXTRA_ARGS+=("-Dfmt_LOCAL_SOURCE=$FMT_LOCAL_SOURCE")
fi
if [[ -n "$GFLAGS_LOCAL_SOURCE" ]]; then
  require_dir "$GFLAGS_LOCAL_SOURCE" 'gflags local source'
  note "gflags local source: $GFLAGS_LOCAL_SOURCE"
  CMAKE_EXTRA_ARGS+=("-Dgflags_LOCAL_SOURCE=$GFLAGS_LOCAL_SOURCE")
fi
if [[ -n "$LIBUNIFEX_LOCAL_SOURCE" ]]; then
  require_dir "$LIBUNIFEX_LOCAL_SOURCE" 'libunifex local source'
  note "libunifex local source: $LIBUNIFEX_LOCAL_SOURCE"
  CMAKE_EXTRA_ARGS+=("-Dlibunifex_LOCAL_SOURCE=$LIBUNIFEX_LOCAL_SOURCE")
fi
if [[ -n "$NLOHMANN_JSON_LOCAL_SOURCE" ]]; then
  require_dir "$NLOHMANN_JSON_LOCAL_SOURCE" 'nlohmann_json local source'
  note "nlohmann_json local source: $NLOHMANN_JSON_LOCAL_SOURCE"
  CMAKE_EXTRA_ARGS+=("-Dnlohmann_json_LOCAL_SOURCE=$NLOHMANN_JSON_LOCAL_SOURCE")
fi
if [[ -n "$TBB_LOCAL_SOURCE" ]]; then
  require_dir "$TBB_LOCAL_SOURCE" 'oneTBB local source'
  note "oneTBB local source: $TBB_LOCAL_SOURCE"
  CMAKE_EXTRA_ARGS+=("-Dtbb_LOCAL_SOURCE=$TBB_LOCAL_SOURCE")
fi
if [[ -n "$YAML_CPP_LOCAL_SOURCE" ]]; then
  require_dir "$YAML_CPP_LOCAL_SOURCE" 'yaml-cpp local source'
  note "yaml-cpp local source: $YAML_CPP_LOCAL_SOURCE"
  CMAKE_EXTRA_ARGS+=("-Dyaml-cpp_LOCAL_SOURCE=$YAML_CPP_LOCAL_SOURCE")
fi

ROS2_PREFIX_LIST_CMAKE="$(prefix_list_to_cmake_list "$PREFIX_LIST")"
ROS2_PREFIX_LIST_COLON="$(prefix_list_to_colon_list "$PREFIX_LIST")"
ROS2_FASTCDR_PREFIX="$(first_prefix_entry "$PREFIX_LIST")"
while [[ ! -f "$ROS2_FASTCDR_PREFIX/lib/libfastcdr.so" && "$ROS2_PREFIX_LIST_COLON" == *:* ]]; do
  ROS2_PREFIX_LIST_COLON="${ROS2_PREFIX_LIST_COLON#*:}"
  ROS2_FASTCDR_PREFIX="${ROS2_PREFIX_LIST_COLON%%:*}"
done

if [[ ! -f "$ROS2_FASTCDR_PREFIX/lib/libfastcdr.so" ]]; then
  fail "ROS2-prefix libfastcdr.so not found in prefix list: $PREFIX_LIST"
fi
ROS2_FASTCDR_LIB="$ROS2_FASTCDR_PREFIX/lib/libfastcdr.so"
ROS2_FASTCDR_INCLUDE_ROOT="$ROS2_FASTCDR_PREFIX/include"
SOURCE_FASTCDR_LIB="$BUILD_DIR/third_party/install/lib/libfastcdr.so"
SOURCE_FASTCDR_INCLUDE_ROOT="$BUILD_DIR/third_party/install/include"

note "repo:        $REPO_ROOT"
note "build:       $BUILD_DIR"
note "preset:      $PRESET_NAME"
note "prefix-list: $PREFIX_LIST"

bash "$REPO_ROOT/scripts/deps/check_target_ros2_prefix.sh" "$PREFIX_LIST" fastdds types rosidl action

mkdir -p "$BUILD_DIR"

if [[ -f "$CACHE_FILE" ]]; then
  EXISTING_GENERATOR="$(cache_value 'CMAKE_GENERATOR')"
  EXISTING_HOME_DIRECTORY="$(cache_value 'CMAKE_HOME_DIRECTORY')"
  note "existing CMake generator: ${EXISTING_GENERATOR:-<unknown>}"
  if [[ "$EXISTING_HOME_DIRECTORY" != "$REPO_ROOT" ]]; then
    fail "existing cache belongs to CMAKE_HOME_DIRECTORY='${EXISTING_HOME_DIRECTORY:-<missing>}', expected exactly '$REPO_ROOT'; existing CMAKE_GENERATOR='${EXISTING_GENERATOR:-<unknown>}'. Use a fresh build directory; the existing tree was not deleted."
  fi
fi

if ! env -u CMAKE_PREFIX_PATH -u COLCON_PREFIX_PATH -u ROS_PACKAGE_PATH \
    PYTHONPATH="$ROS2_PYTHONPATH" \
    AMENT_PREFIX_PATH="$ROS2_PREFIX_LIST_COLON" \
    PKG_CONFIG_LIBDIR="$OPENSSL_ROOT_DIR/lib/pkgconfig" \
    PKG_CONFIG_PATH= \
    cmake -S "$REPO_ROOT" --preset "$PRESET_NAME" \
      -B "$BUILD_DIR" \
      -U'_OPENSSL_*' \
      -U'pkgcfg_lib__OPENSSL_*' \
      -DIBMW_ENABLE_THIRD_PARTY_SOURCE_BUILD=OFF \
      -DIBMW_THIRD_PARTY_SOURCE_BUILD_ONLY=OFF \
      -DIBMW_THIRD_PARTY_SOURCE_PREFIX="$BUILD_DIR/third_party/install" \
      -DIBMW_THIRD_PARTY_SOURCE_BASE_DIR="$BUILD_DIR/third_party/build" \
      -DIBMW_BUILD_PROTOCOLS=ON \
      -DIBMW_BUILD_EXAMPLES=OFF \
      -DIBMW_BUILD_DDS_EXTENSION=ON \
      -DIBMW_BUILD_ROS2_TYPES=ON \
      -DIBMW_BUILD_WITH_ROS2=OFF \
      -DIBMW_BUILD_ROS2_EXTENSION=OFF \
      -DIBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES=ON \
      -DIBMW_TARGET_ROS2_TIER=rosidl \
      -DIBMW_TARGET_ROS2_DEPS_PREFIX="$ROS2_PREFIX_LIST_CMAKE" \
      -DOHOS_SDK_NATIVE_ROOT="$SDK_NATIVE_ROOT" \
      -DPYTHON_EXECUTABLE="$PYTHON_EXECUTABLE" \
      -DPython3_EXECUTABLE="$PYTHON_EXECUTABLE" \
      -DPYTHON_INCLUDE_DIR="$PYTHON_INCLUDE_DIR" \
      -DPYTHON_LIBRARY="$PYTHON_LIBRARY" \
      -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT_DIR" \
      -DOPENSSL_INCLUDE_DIR="$OPENSSL_INCLUDE_DIR" \
      -DOPENSSL_SSL_LIBRARY="$OPENSSL_SSL_LIBRARY" \
      -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_CRYPTO_LIBRARY" \
      "${CMAKE_EXTRA_ARGS[@]}" \
      >"$CONFIGURE_LOG" 2>&1; then
  echo "[ohos-rosidl-compile] configure log: $CONFIGURE_LOG" >&2
  exit 2
fi

CONFIGURED_GENERATOR="$(cache_value 'CMAKE_GENERATOR')"
note "configured CMake generator: ${CONFIGURED_GENERATOR:-<unknown>}"

require_cache_value 'IBMW_THIRD_PARTY_SOURCE_BUILD_ONLY' 'OFF'
require_cache_value 'IBMW_BUILD_PROTOCOLS' 'ON'
require_cache_value 'IBMW_BUILD_EXAMPLES' 'OFF'
require_cache_value 'IBMW_BUILD_DDS_EXTENSION' 'ON'
require_cache_value 'IBMW_BUILD_ROS2_TYPES' 'ON'
require_cache_value 'IBMW_BUILD_WITH_ROS2' 'OFF'
require_cache_value 'IBMW_BUILD_ROS2_EXTENSION' 'OFF'
require_cache_value 'IBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES' 'ON'
require_cache_value 'IBMW_TARGET_ROS2_TIER' 'rosidl'

if ! env -u CMAKE_PREFIX_PATH -u COLCON_PREFIX_PATH -u ROS_PACKAGE_PATH \
    PYTHONPATH="$ROS2_PYTHONPATH" \
    AMENT_PREFIX_PATH="$ROS2_PREFIX_LIST_COLON" \
    cmake --build "$BUILD_DIR" \
      --target ibmw_schema_ibmw_msgs_ts ibmw_extensions_ros2_types_impl ibmw_dds_extension ibmw_fibonacci_action_dds_server ibmw_fibonacci_action_dds_client \
      -j"$(nproc)" \
      >"$BUILD_LOG" 2>&1; then
  echo "[ohos-rosidl-compile] build log: $BUILD_LOG" >&2
  exit 2
fi

require_file "$LINK_FILE" 'ibmw_schema_ibmw_msgs_ts link line'
require_file_contains "$LINK_FILE" "$ROS2_FASTCDR_LIB" 'ibmw_schema_ibmw_msgs_ts link line'
require_file_not_contains "$LINK_FILE" "$SOURCE_FASTCDR_LIB" 'ibmw_schema_ibmw_msgs_ts link line'

for link_file in "$FASTRTPS_C_LINK_FILE" "$FASTRTPS_CPP_LINK_FILE"; do
  require_file "$link_file" 'generated FastRTPS link line'
  require_file_contains "$link_file" "$ROS2_FASTCDR_LIB" 'generated FastRTPS link line'
  require_file_not_contains "$link_file" "$SOURCE_FASTCDR_LIB" 'generated FastRTPS link line'
done

require_fastrtps_c_include_provenance \
  "$ROS2_FASTCDR_INCLUDE_ROOT" \
  "$SOURCE_FASTCDR_INCLUDE_ROOT"

declare -a ARTIFACTS=("$CACHE_FILE" "$CONFIGURE_LOG" "$BUILD_LOG" "$LINK_FILE" "$FASTRTPS_C_LINK_FILE" "$FASTRTPS_CPP_LINK_FILE")
check_forbidden_path_leakage '/opt/ros/humble' "${ARTIFACTS[@]}"
check_forbidden_path_leakage '/usr/lib/x86_64-linux-gnu' "${ARTIFACTS[@]}"

bash "$ROUTE_B_LINK_BOUNDARY_CHECKER" "$BUILD_DIR"

pass 'OHOS ARM64 ROS2 rosidl + private DDS-only Action Fibonacci compile/link slice validated.'
