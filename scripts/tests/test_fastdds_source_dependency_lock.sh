#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Static lock test for the two supported Fast DDS dependency closures.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GET_FASTDDS="$REPO_ROOT/cmake/GetFastDDS.cmake"
TOP_LEVEL="$REPO_ROOT/CMakeLists.txt"
SOURCE_FASTCDR="$REPO_ROOT/cmake/IbmwSourceFastCDR.cmake"
SOURCE_FASTDDS="$REPO_ROOT/cmake/IbmwSourceFastDDS.cmake"
SOURCE_FOONATHAN="$REPO_ROOT/cmake/IbmwSourceFoonathanMemory.cmake"
SOURCE_TINYXML2="$REPO_ROOT/cmake/IbmwSourceTinyXML2.cmake"
SOURCE_ASIO="$REPO_ROOT/cmake/IbmwSourceAsio.cmake"
SOURCE_HELPER="$REPO_ROOT/cmake/IbmwThirdPartySourceBuild.cmake"
GET_CPPTOML="$REPO_ROOT/cmake/GetCppToml.cmake"
GET_GFLAGS="$REPO_ROOT/cmake/GetGFlags.cmake"
CPPTOML_PATCH="$REPO_ROOT/cmake/patches/cpptoml-0.1.0-limits.patch"
CONFIG_TEMPLATE="$REPO_ROOT/cmake/ibmw-config.cmake.in"

assert_contains() {
  local file="$1"
  local text="$2"
  if ! grep -Fq "$text" "$file"; then
    printf 'missing expected text in %s: %s\n' "$file" "$text" >&2
    exit 1
  fi
}

assert_absent() {
  local file="$1"
  local text="$2"
  if grep -Fq "$text" "$file"; then
    printf 'unexpected text in %s: %s\n' "$file" "$text" >&2
    exit 1
  fi
}

assert_contains "$TOP_LEVEL" 'SYSTEM ROS2_MINIMAL_SDK SOURCE_FASTDDS2 SOURCE_FASTDDS3'
assert_contains "$TOP_LEVEL" 'IBMW_DDS_PROVIDER MATCHES "^SOURCE_FASTDDS[23]$"'
assert_contains "$TOP_LEVEL" 'add_test(NAME ibmw_fastdds_source_dependency_lock'
assert_contains "$SOURCE_FASTCDR" 'codeload.github.com/eProsima/Fast-CDR/tar.gz/959ff6cc637c940d3deca3d8398920242120daee'
assert_contains "$SOURCE_FASTCDR" 'SHA256=c79a9b68df7abe9fed7f897f8c9f5168e46a1835b8c75a9c8ee309784df90366'
assert_contains "$SOURCE_FASTDDS" 'codeload.github.com/eProsima/Fast-DDS/tar.gz/ac5c7b9014cd5b2bebf9c4b04dc229ca5a0a4338'
assert_contains "$SOURCE_FASTDDS" 'SHA256=017401b2e6749ca382418b08474423c6bf35b8cdc489c69a4aff3b9401231305'
assert_contains "$SOURCE_FOONATHAN" 'codeload.github.com/foonathan/memory/tar.gz/19ab0759c7f053d88657c0eb86d879493f784d61'
assert_contains "$SOURCE_FOONATHAN" 'SHA256=e117180f5dc8e335f746330e2c9f69a55ccf76e2d6a5bc7947d2d35afd91cd37'

# The Fast DDS 3 closure remains independently pinned.
assert_contains "$SOURCE_FASTCDR" 'tags/v2.3.0.tar.gz'
assert_contains "$SOURCE_FASTCDR" 'SHA256=d85ee9e24e105581b49de3e2b6b2585335a8dc98c4cabd288232fffc4b9b6a24'
assert_contains "$SOURCE_FASTDDS" 'tags/v3.2.2.tar.gz'
assert_contains "$SOURCE_FASTDDS" 'SHA256=243699d3b2f9a97012d74c3369abe705aaab0700712f3d9ecbd9f705a336b044'
assert_contains "$SOURCE_FOONATHAN" 'tags/v0.7-3.tar.gz'
assert_contains "$SOURCE_FOONATHAN" 'SHA256=4203d15db22a94a3978eeb1afb59a37d35c57c0f148733f0f1a53a6281cb74dd'
assert_contains "$SOURCE_TINYXML2" 'SHA256=3bdf15128ba16686e69bce256cc468e76c7b94ff2c7f391cc5ec09e40bff3839'
assert_contains "$SOURCE_ASIO" 'SHA256=755bd7f85a4b269c67ae0ea254907c078d408cce8e1a352ad2ed664d233780e8'
assert_contains "$SOURCE_HELPER" 'requires URL_HASH when downloading from URL.'
assert_contains "$SOURCE_HELPER" 'set(_download_args URL "${_IBMW_DEP_URL}" URL_HASH "${_IBMW_DEP_URL_HASH}"'

# Fast DDS transitive source dependencies are immutable and auditable.
assert_contains "$GET_CPPTOML" 'URL_HASH SHA256=11b1c07132d23e85071cef3d0fe9bc605de731fcd1b1c3f3d43ed09a6fd7a850'
assert_contains "$GET_CPPTOML" 'PATCH_COMMAND'
assert_contains "$GET_CPPTOML" 'patches/cpptoml-0.1.0-limits.patch'
assert_contains "$GET_CPPTOML" 'set(CPPTOML_BUILD_EXAMPLES OFF CACHE BOOL'
assert_contains "$GET_CPPTOML" 'set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)'
assert_absent "$GET_CPPTOML" 'string(REPLACE'
assert_contains "$CPPTOML_PATCH" ' #include <cstring>'
assert_contains "$CPPTOML_PATCH" '+#include <limits>'
assert_contains "$GET_GFLAGS" 'URL_HASH SHA256=34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf'

# Fast-CDR 1.0.29 has no Fast-CDR 2.x optional implementation. The obsolete
# 2.2.6 patch stays removed; this test does not claim that its guard was ported.
assert_absent "$SOURCE_FASTCDR" '2.2.6'
assert_absent "$SOURCE_FASTCDR" 'PATCH_COMMAND'
assert_contains "$GET_FASTDDS" 'find_package(fastcdr 2.3.0 EXACT REQUIRED CONFIG PATHS'
assert_contains "$GET_FASTDDS" '_ibmw_require_version("Fast CDR" "${fastcdr_VERSION}" "2.3.0")'
assert_contains "$GET_FASTDDS" '_ibmw_require_version("foonathan_memory" "${foonathan_memory_VERSION}" "0.7.3")'
assert_contains "$GET_FASTDDS" 'find_package(fastdds 3.2.2 EXACT REQUIRED CONFIG PATHS'
assert_contains "$GET_FASTDDS" 'foreach(_ibmw_target IN ITEMS fastcdr fastdds foonathan_memory)'
assert_contains "$GET_FASTDDS" '_ibmw_validate_imported_target_under_prefix(${_ibmw_target} "${IBMW_THIRD_PARTY_SOURCE_PREFIX}")'
assert_contains "$CONFIG_TEMPLATE" 'find_package(fastcdr 2.3.0 EXACT REQUIRED CONFIG PATHS'
assert_contains "$CONFIG_TEMPLATE" '_ibmw_config_validate_target(${_target} "${IBMW_THIRD_PARTY_SOURCE_PREFIX}")'
assert_contains "$CONFIG_TEMPLATE" 'EXISTS "${_IBMW_PACKAGE_PREFIX_DIR}/share/fastdds/cmake"'

assert_contains "$GET_FASTDDS" 'elseif(IBMW_DDS_PROVIDER STREQUAL "SOURCE_FASTDDS2")'
assert_contains "$GET_FASTDDS" 'find_package(fastcdr 1.0.29 EXACT REQUIRED CONFIG PATHS "${IBMW_THIRD_PARTY_SOURCE_PREFIX}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)'
assert_contains "$GET_FASTDDS" 'find_package(fastrtps 2.6.12 EXACT REQUIRED CONFIG PATHS "${IBMW_THIRD_PARTY_SOURCE_PREFIX}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)'
assert_contains "$GET_FASTDDS" 'set(IBMW_FASTDDS_PROVIDER_TARGET fastrtps)'
assert_contains "$CONFIG_TEMPLATE" 'elseif(_IBMW_BUILT_DDS_PROVIDER STREQUAL "SOURCE_FASTDDS2")'
assert_contains "$CONFIG_TEMPLATE" 'EXISTS "${_IBMW_PACKAGE_PREFIX_DIR}/share/fastrtps/cmake"'
assert_contains "$CONFIG_TEMPLATE" 'find_package(fastrtps 2.6.12 EXACT REQUIRED CONFIG PATHS "${IBMW_THIRD_PARTY_SOURCE_PREFIX}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)'
assert_contains "$CONFIG_TEMPLATE" 'set(IBMW_FASTDDS_PROVIDER_TARGET fastrtps)'

# The ROS2 Humble lane remains independently locked to its 2.6.12/1.0.29 ABI.
assert_contains "$GET_FASTDDS" 'find_package(fastcdr 1.0.29 EXACT CONFIG QUIET PATHS'
assert_contains "$GET_FASTDDS" 'find_package(fastrtps 2.6.12 EXACT CONFIG QUIET PATHS'

printf 'PASS: Fast DDS source closures are locked for 2.6.12/1.0.29/0.7.1 and 3.2.2/2.3.0/0.7.3.\n'
