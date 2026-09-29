#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Compile generated DDS codecs and optionally a ROS2 imported-service consumer.
# Usage: test_text_typesupport_consumers.sh -DCMAKE_PREFIX_PATH=<sdk> [CMake options]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
cmake -S "$SCRIPT_DIR/fixtures/text_typesupport" -B "$SCRATCH" "$@"
cmake --build "$SCRATCH" --parallel "${IBMW_TEST_JOBS:-2}"
ctest --test-dir "$SCRATCH" --output-on-failure
