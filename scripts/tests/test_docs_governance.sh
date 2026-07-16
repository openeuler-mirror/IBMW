#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CHECKER="$REPO_ROOT/scripts/check_docs_governance.py"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

init_fixture() {
  local root="$1"
  mkdir -p \
    "$root/docs/guides" \
    "$root/docs/reference" \
    "$root/docs/contributing" \
    "$root/docs/ecosystem" \
    "$root/docs/releases" \
    "$root/guide"
  git -C "$root" init -q
  git -C "$root" config user.name "Governance Fixture"
  git -C "$root" config user.email "governance@example.invalid"

  local file
  for file in \
    README.md README.zh.md CONTRIBUTING.md CHANGELOG.md SECURITY.md SUPPORT.md \
    CODE_OF_CONDUCT.md LICENSE NOTICE \
    docs/README.md docs/guides/user-guide.md \
    docs/reference/api-reference.md docs/reference/compatibility.md \
    docs/reference/support-matrix.md docs/contributing/testing.md \
    docs/contributing/architecture-rules.md docs/ecosystem/README.md \
    docs/releases/0.1.0.md; do
    printf '# Current governance\n' > "$root/$file"
  done
  printf '# Guide\n' > "$root/guide/README.md"
  cat >> "$root/docs/README.md" <<'EOF'
[guide](../guide/README.md#start)
[directory](../guide/)
[web](https://example.invalid/docs)
[mail](mailto:maintainer@example.invalid)
[anchor](#local)
EOF
  git -C "$root" add .
  git -C "$root" commit -qm "fixture baseline"
}

GOOD="$SCRATCH/good"
mkdir -p "$GOOD"
init_fixture "$GOOD"
python3 "$CHECKER" --check --repo-root "$GOOD" >/dev/null \
  || fail "valid documentation fixture was rejected"

printf '\n[missing](does-not-exist.md)\n' >> "$GOOD/docs/README.md"
if python3 "$CHECKER" --check --repo-root "$GOOD" >"$SCRATCH/dead.out" 2>&1; then
  fail "broken local link was accepted"
fi
grep -q "broken local link" "$SCRATCH/dead.out" \
  || fail "broken-link failure did not identify the problem"
git -C "$GOOD" restore docs/README.md

path_separator="/"
stale_cases=(
  "${path_separator}home${path_separator}oee${path_separator}private"
  "${path_separator}go${path_separator}private"
  "${path_separator}tmp${path_separator}opencode${path_separator}private"
  ".sisyphus/private"
  "IBMW_VERIFICATION_NOTES.md"
  "PROJECT_STATUS.md"
  "REFACTOR_PLAN_Y9"
  "docs/data/private.csv"
)
for stale in "${stale_cases[@]}"; do
  printf '\nPrivate reference: `%s`\n' "$stale" >> "$GOOD/docs/README.md"
  if python3 "$CHECKER" --check --repo-root "$GOOD" >"$SCRATCH/stale.out" 2>&1; then
    fail "stale private reference was accepted: $stale"
  fi
  grep -q "stale private reference" "$SCRATCH/stale.out" \
    || fail "stale-reference failure did not identify the problem: $stale"
  git -C "$GOOD" restore docs/README.md
done

printf '\nFixture: `%s` <!-- docs-governance: allow-private-reference -->\n' \
  "${stale_cases[0]}" >> "$GOOD/docs/README.md"
python3 "$CHECKER" --check --repo-root "$GOOD" >/dev/null \
  || fail "explicitly marked stale-reference fixture was rejected"
git -C "$GOOD" restore docs/README.md

rm "$GOOD/docs/reference/compatibility.md"
if python3 "$CHECKER" --check-deletions HEAD --repo-root "$GOOD" >"$SCRATCH/delete.out" 2>&1; then
  fail "protected document deletion was accepted"
fi
grep -q "protected document deleted or renamed" "$SCRATCH/delete.out" \
  || fail "deletion failure did not identify the protected path"

git -C "$GOOD" restore docs/reference/compatibility.md
git -C "$GOOD" mv docs/contributing/testing.md docs/contributing/testing-renamed.md
if python3 "$CHECKER" --check-deletions HEAD --repo-root "$GOOD" >"$SCRATCH/rename.out" 2>&1; then
  fail "protected document rename was accepted"
fi
grep -q "protected document deleted or renamed" "$SCRATCH/rename.out" \
  || fail "rename failure did not identify the protected path"

git -C "$GOOD" mv docs/contributing/testing-renamed.md docs/contributing/testing.md
rm "$GOOD/NOTICE"
if python3 "$CHECKER" --check --repo-root "$GOOD" >"$SCRATCH/required.out" 2>&1; then
  fail "missing required public authority was accepted"
fi
grep -q "required document is missing: NOTICE" "$SCRATCH/required.out" \
  || fail "required-document failure did not identify NOTICE"
git -C "$GOOD" restore NOTICE

mkdir -p "$GOOD/docs/history" "$GOOD/skills/migration" "$GOOD/skills/sdk"
legacy_documents=(
  AGENTS.md
  docs/PROJECT_STATUS.md
  docs/compatibility.md
  docs/troubleshooting.md
  docs/history/commit-context.md
  docs/IBMW_VERIFICATION_NOTES.md
  docs/IBMW_VERIFICATION_NOTES_V2.md
  docs/IBMW_VERIFICATION_NOTES_V3.md
  skills/README.md
  skills/transport_decoupling.md
  skills/ibmw_testing.md
  skills/migration/SKILL.md
  skills/sdk/SKILL.md
)
for legacy in "${legacy_documents[@]}"; do
  printf '# Legacy fixture\n' > "$GOOD/$legacy"
done
git -C "$GOOD" add .
git -C "$GOOD" commit -qm "add legacy non-authorities"
rm "${legacy_documents[@]/#/$GOOD/}"
python3 "$CHECKER" --check-deletions HEAD --repo-root "$GOOD" >/dev/null \
  || fail "deletion of legacy non-authority documents was rejected"

echo "PASS: docs governance authorities, links, private references, deletion, and rename fixtures"
