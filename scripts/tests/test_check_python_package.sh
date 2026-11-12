#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
# cmake/CheckPythonPackage.cmake must detect importable modules without pip,
# e.g. distribution packages in a venv created --without-pip (TOOL-047).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

python3 -m venv --without-pip "$SCRATCH/venv"
mkdir -p "$SCRATCH/site/ibmw_probe_module"
touch "$SCRATCH/site/ibmw_probe_module/__init__.py"
cat > "$SCRATCH/probe.cmake" <<CMAKE
set(Python3_EXECUTABLE "$SCRATCH/venv/bin/python")
include("$REPO_ROOT/cmake/CheckPythonPackage.cmake")
check_python_package(ibmw_probe_module PRESENT)
check_python_package(ibmw_missing_module ABSENT)
message(STATUS "present=\${PRESENT} absent=\${ABSENT}")
CMAKE

"$SCRATCH/venv/bin/python" -m pip --version >/dev/null 2>&1 && fail "fixture venv unexpectedly has pip"
output="$(PYTHONPATH="$SCRATCH/site" cmake -P "$SCRATCH/probe.cmake" 2>&1)"
grep -q "present=ON absent=OFF" <<<"$output" || fail "unexpected detection result: $output"
grep -q "Cannot import ibmw_missing_module" <<<"$output" || fail "missing module was not reported"

echo "PASS: check_python_package detects importable modules without pip"
