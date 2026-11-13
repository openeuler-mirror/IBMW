#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
HOOK="$REPO_ROOT/githooks/commit-msg"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

run_message() {
  local name="$1"
  shift
  printf '%s\n' "$@" > "$SCRATCH/$name"
  "$HOOK" "$SCRATCH/$name"
}

run_message valid \
  "feat(governance): add local documentation policy checks" \
  "" \
  "What: Add deterministic repository governance checks." \
  "Why: Prevent documentation and commit-policy drift." \
  "Verification: Run the shell fixture suite." \
  || fail "valid non-trivial commit was rejected"

run_message minor "docs(readme): fix spelling" \
  || fail "minor documentation commit was rejected"

run_message automatic-revert 'Revert "feat(core): temporary change"' \
  || fail "automatic revert message was rejected"

run_message fixup "fixup! feat(core): add transport" \
  || fail "fixup message was rejected"

run_message automatic-merge "Merge branch 'feature/x' into main" \
  || fail "automatic merge message was rejected"

# Only git-generated prefixes are exempt: "merge"/"Merge" followed by free text is not.
for subject in "merge fix for the transport" "Merge the transport fix" "MERGE branch 'x'"; do
  if run_message manual-merge "$subject" >"$SCRATCH/merge.out" 2>&1; then
    fail "manual subject '$subject' bypassed validation"
  fi
done

if run_message missing-body "fix(core): correct transport state" >"$SCRATCH/body.out" 2>&1; then
  fail "non-trivial commit without body was accepted"
fi
grep -q "What:.*Why:.*Verification:" "$SCRATCH/body.out" \
  || fail "missing-body failure did not list required sections"

if run_message invalid "feature(core): invalid type" >"$SCRATCH/invalid.out" 2>&1; then
  fail "invalid conventional subject was accepted"
fi
grep -q "must match" "$SCRATCH/invalid.out" \
  || fail "invalid-subject failure was unclear"

long_subject="$(printf 'x%.0s' {1..73})"
if run_message too-long "chore(scope): $long_subject" >"$SCRATCH/long.out" 2>&1; then
  fail "subject longer than 72 characters was accepted"
fi
grep -q "maximum is 72" "$SCRATCH/long.out" \
  || fail "long-subject failure was unclear"

echo "PASS: commit-msg valid, exempt, missing-body, subject, and length fixtures"
