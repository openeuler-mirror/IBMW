#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
# Local merge gate (HYG-044): runs, one after another, the build, unit tests,
# release hygiene, docs governance, clang-format of the listed files and
# generated-sample drift on the committed tree, and commit-message validation of every commit on the branch. Steps run
# serially and parallelism is capped so the gate can run on a shared machine.
#
# Usage: scripts/ci_check.sh [--build-dir DIR] [--base REF] [--skip-build]
#   --build-dir DIR  CMake build directory (default: <repo>/build/ci). An
#                    existing configured directory is reused as is; a new one
#                    is configured with IBMW_CI_CMAKE_ARGS (default: tests ON).
#   --base REF       Commits in REF..HEAD are checked (default: the upstream
#                    branch, else master, else main).
#   --skip-build     Skip build and unit tests (policy checks only).
# Environment: IBMW_CI_JOBS (build jobs, default 10), IBMW_CI_TEST_JOBS
# (ctest jobs, default 8), IBMW_CI_CMAKE_ARGS (extra configure arguments),
# CLANG_FORMAT (clang-format 18, default: clang-format on PATH).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build/ci"
BASE=""
SKIP_BUILD=0
JOBS="${IBMW_CI_JOBS:-10}"
TEST_JOBS="${IBMW_CI_TEST_JOBS:-8}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --base) BASE="$2"; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    -h|--help) sed -n '3,17p' "$0"; exit 0 ;;
    *) echo "ci_check: unknown argument: $1" >&2; exit 2 ;;
  esac
done

step() { echo; echo "==> $*"; }

if [[ "$SKIP_BUILD" -eq 0 ]]; then
  step "build ($BUILD_DIR, -j$JOBS)"
  if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    # shellcheck disable=SC2086  # IBMW_CI_CMAKE_ARGS is a list of arguments.
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja -DIBMW_BUILD_TESTS=ON ${IBMW_CI_CMAKE_ARGS:-}
  fi
  ninja -j"$JOBS" -C "$BUILD_DIR"

  step "unit tests (-j$TEST_JOBS)"
  ctest --test-dir "$BUILD_DIR" -j"$TEST_JOBS" --timeout 300 --output-on-failure
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

step "release hygiene and docs governance (committed tree)"
# The work tree holds ignored build and tool output; check what is committed.
git -C "$REPO_ROOT" archive HEAD | tar -x -C "$SCRATCH" --one-top-level=tree
python3 "$SCRIPT_DIR/check_release_hygiene.py" --repo-root "$SCRATCH/tree"
python3 "$SCRIPT_DIR/check_docs_governance.py" --check --repo-root "$REPO_ROOT"
bash "$SCRIPT_DIR/tests/test_docs_governance.sh"
bash "$SCRIPT_DIR/tests/test_release_hygiene.sh"
bash "$SCRIPT_DIR/tests/test_commit_msg_hook.sh"
bash "$SCRIPT_DIR/tests/test_check_python_package.sh"
bash "$SCRIPT_DIR/tests/test_check_clang_format.sh"

step "clang-format of the listed files (committed tree)"
bash "$SCRIPT_DIR/check_clang_format.sh" --repo-root "$SCRATCH/tree"

step "generated samples match the generator (committed tree)"
python3 "$SCRATCH/tree/src/tools/ibmw_gen/tests/test_in_tree_samples_no_drift.py"

step "commit messages"
if [[ -z "$BASE" ]]; then
  for candidate in '@{upstream}' master main; do
    if git -C "$REPO_ROOT" rev-parse --verify -q "$candidate^{commit}" >/dev/null 2>&1; then
      BASE="$candidate"
      break
    fi
  done
fi
if [[ -z "$BASE" ]]; then
  echo "ci_check: no base found; pass --base REF" >&2
  exit 2
fi
python3 "$SCRIPT_DIR/check_docs_governance.py" --check-deletions "$BASE...HEAD" --repo-root "$REPO_ROOT"
count=0
while read -r commit; do
  git -C "$REPO_ROOT" log -1 --format=%B "$commit" > "$SCRATCH/msg"
  if ! "$REPO_ROOT/githooks/commit-msg" "$SCRATCH/msg"; then
    echo "ci_check: commit $(git -C "$REPO_ROOT" log -1 --format='%h %s' "$commit")" >&2
    exit 1
  fi
  count=$((count + 1))
done < <(git -C "$REPO_ROOT" rev-list "$BASE..HEAD")
echo "commit messages: $count checked against $BASE"

echo
echo "PASS: ci_check"
