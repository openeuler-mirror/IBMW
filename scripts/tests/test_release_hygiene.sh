#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CHECKER="$REPO_ROOT/scripts/check_release_hygiene.py"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

new_fixture() {
  local name="$1"
  local root="$SCRATCH/$name"
  mkdir -p "$root"
  git -C "$root" init -q
  printf '# Clean public fixture\n' > "$root/README.md"
  printf '%s\n' "$root"
}

expect_rejected() {
  local root="$1"
  local expected="$2"
  local output="$SCRATCH/rejected.out"
  if python3 "$CHECKER" --repo-root "$root" >"$output" 2>&1; then
    fail "release hygiene accepted forbidden fixture: $expected"
  fi
  grep -q "$expected" "$output" \
    || fail "failure did not identify forbidden fixture: $expected"
}

CLEAN="$(new_fixture clean)"
python3 "$CHECKER" --repo-root "$CLEAN" >/dev/null \
  || fail "clean work tree was rejected"

for cache in .pytest_cache __pycache__ .sisyphus; do
  FIXTURE="$(new_fixture "cache-${cache//[^A-Za-z0-9]/-}")"
  printf '%s/\n' "$cache" > "$FIXTURE/.gitignore"
  mkdir -p "$FIXTURE/$cache"
  printf 'ignored cache\n' > "$FIXTURE/$cache/item"
  expect_rejected "$FIXTURE" "$cache"
done

for cache_file in module.pyc module.pyo; do
  FIXTURE="$(new_fixture "cache-$cache_file")"
  printf '*.py[co]\n' > "$FIXTURE/.gitignore"
  printf 'cache\n' > "$FIXTURE/$cache_file"
  expect_rejected "$FIXTURE" "$cache_file"
done

for swap_file in .README.md.swp .README.md.swo .README.md.swn; do
  FIXTURE="$(new_fixture "swap-${swap_file##*.}")"
  printf '*.sw?\n' > "$FIXTURE/.gitignore"
  printf 'swap\n' > "$FIXTURE/$swap_file"
  expect_rejected "$FIXTURE" "$swap_file"
done

path_separator="/"
machine_paths=(
  "${path_separator}home${path_separator}oee${path_separator}private"
  "${path_separator}home${path_separator}public-user${path_separator}project"
  "${path_separator}path${path_separator}to${path_separator}sdk"
  "${path_separator}tmp${path_separator}opencode${path_separator}private"
  "${path_separator}go${path_separator}ibmw${path_separator}private"
)
for value in "${machine_paths[@]}"; do
  FIXTURE="$(new_fixture machine-path)"
  printf 'Private path: %s\n' "$value" >> "$FIXTURE/README.md"
  expect_rejected "$FIXTURE" "forbidden release content"
done

governance_sources=(
  "scripts/check_docs_governance.py"
  "scripts/check_release_hygiene.py"
  "scripts/tests/test_docs_governance.sh"
  "scripts/tests/test_release_hygiene.sh"
)
for source in "${governance_sources[@]}"; do
  FIXTURE="$(new_fixture governance-source-path)"
  mkdir -p "$(dirname "$FIXTURE/$source")"
  printf 'Private path: %s\n' "${machine_paths[0]}" > "$FIXTURE/$source"
  expect_rejected "$FIXTURE" "forbidden release content"
done

internal_markers=(
  "IBMW_VERIFICATION_NOTES"
  "PROJECT_STATUS"
  "REFACTOR_PLAN_Y9"
  "docs/data/private.csv"
  "HR-17"
  "hr-17"
  "Hr-17"
  "QW-alpha"
  "qw-alpha"
  "qW-alpha"
  "§Y9"
  "§y9"
  "V2 NOTES"
  "THIS COMMIT"
)
for value in "${internal_markers[@]}"; do
  FIXTURE="$(new_fixture internal-marker)"
  printf 'Internal marker: %s\n' "$value" >> "$FIXTURE/README.md"
  expect_rejected "$FIXTURE" "forbidden release content"
done

legacy_ownership_markers=(
  "openEuler Embedded SIG & IB-Robot Group"
  "The Ibmw Authors"
  "openEuler Embedded SIG &amp; IB-Robot Group"
)
for value in "${legacy_ownership_markers[@]}"; do
  FIXTURE="$(new_fixture legacy-ownership)"
  printf 'Copyright: %s\n' "$value" >> "$FIXTURE/README.md"
  expect_rejected "$FIXTURE" "forbidden release content"
done

echo "PASS: release hygiene cache, swap file, private path, internal milestone, legacy ownership, and clean fixtures"
