#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Exercise default non-ROS installation, build-tree and relocated SDK consumers.
# Extra CMake arguments can select cached dependency sources for offline runs.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
cmake -S "$REPO_ROOT" -B "$SCRATCH/build" \
  -DIBMW_BUILD_RUNTIME=OFF -DIBMW_BUILD_PROTOCOLS=OFF -DIBMW_BUILD_TESTS=OFF \
  -DIBMW_BUILD_WITH_ROS2=OFF -DIBMW_BUILD_ROS2_TYPES=OFF \
  -DIBMW_BUILD_EXAMPLES=OFF -DIBMW_BUILD_CLI_TOOLS=OFF \
  -DCMAKE_INSTALL_PREFIX="$SCRATCH/install" "$@"
cmake --build "$SCRATCH/build" --parallel "${IBMW_TEST_JOBS:-2}"
cmake --install "$SCRATCH/build"
mv "$SCRATCH/install" "$SCRATCH/relocated"
mkdir "$SCRATCH/consumer"
cat >"$SCRATCH/consumer/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(json_sdk_consumer LANGUAGES CXX)
find_package(ibmw REQUIRED CONFIG)
add_executable(consumer main.cc)
target_compile_features(consumer PRIVATE cxx_std_20)
target_link_libraries(consumer PRIVATE ibmw::api::type_support)
EOF
cat >"$SCRATCH/consumer/main.cc" <<'EOF'
#include "type_support/text_serialization.h"
int main() {
  const auto value = ibmw::util::text::Json::parse("{\"value\":42}");
  return value.at("value") == 42 ? 0 : 1;
}
EOF
for kind in build-tree-config relocated; do
  prefix="$SCRATCH/$kind"
  if [[ "$kind" == build-tree-config ]]; then prefix="$SCRATCH/build/$kind"; fi
  cmake -S "$SCRATCH/consumer" -B "$SCRATCH/consumer-$kind" -DCMAKE_PREFIX_PATH="$prefix"
  cmake --build "$SCRATCH/consumer-$kind" --parallel "${IBMW_TEST_JOBS:-2}"
  "$SCRATCH/consumer-$kind/consumer"
done
