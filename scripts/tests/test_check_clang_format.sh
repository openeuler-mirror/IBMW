#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# scripts/check_clang_format.sh passes clean files and fails on unformatted
# or missing ones, a missing tool, or another clang-format version (HYG-028).
# A stub stands in for clang-format, so the real tool is not needed.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECK="$SCRIPT_DIR/../check_clang_format.sh"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

# The stub reports clang-format $STUB_VERSION and rejects files containing "BAD".
cat > "$SCRATCH/clang-format" <<'STUB'
#!/usr/bin/env bash
if [[ "$1" == "--version" ]]; then
  echo "clang-format version ${STUB_VERSION:-18.1.8}"
  exit 0
fi
! grep -q BAD "${@: -1}"
STUB
chmod +x "$SCRATCH/clang-format"

repo="$SCRATCH/repo"
mkdir -p "$repo/scripts" "$repo/src"
echo "int a;" > "$repo/src/good.cc"
echo "BAD" > "$repo/src/bad.cc"
run() {
  printf '# comment\n\n%s\n' "$@" > "$repo/scripts/clang_format_files.txt"
  CLANG_FORMAT="$SCRATCH/clang-format" bash "$CHECK" --repo-root "$repo" >/dev/null 2>&1
}

run src/good.cc || fail "clean file rejected"
rc=0; run src/good.cc src/bad.cc || rc=$?
[[ "$rc" == 1 ]] || fail "unformatted file not rejected (rc=$rc)"
rc=0; run src/missing.cc || rc=$?
[[ "$rc" == 1 ]] || fail "missing file not rejected (rc=$rc)"
rc=0; STUB_VERSION=17.0.6 run src/good.cc || rc=$?
[[ "$rc" == 2 ]] || fail "other version accepted (rc=$rc)"
rc=0; printf 'src/good.cc\n' > "$repo/scripts/clang_format_files.txt"
CLANG_FORMAT="$SCRATCH/none" bash "$CHECK" --repo-root "$repo" >/dev/null 2>&1 || rc=$?
[[ "$rc" == 2 ]] || fail "missing tool not reported (rc=$rc)"

echo "PASS: check_clang_format clean, unformatted, missing file, version, and missing tool fixtures"
