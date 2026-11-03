#!/usr/bin/env bash
# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# TOOL-041: each cmake_flags item of the matrix yaml reaches cmake as one
# argument, without word splitting or pathname expansion. A stub cmake
# records the arguments and fails, so nothing is built; --report keeps the
# expected build failure from failing the run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

mkdir -p "$SCRATCH/bin" "$SCRATCH/root" "$SCRATCH/cwd"
# Matches the -DGLOB=* flag if it were expanded as a pattern.
touch "$SCRATCH/cwd/-DGLOB=planted"
cat > "$SCRATCH/bin/cmake" <<STUB
#!/usr/bin/env bash
for a in "\$@"; do printf '%s\n' "\$a"; done | grep -E '^(-DGLOB|-DSPACE|-DOTHER|b\$)' >> "$SCRATCH/args.txt" || true
exit 1
STUB
chmod +x "$SCRATCH/bin/cmake"
cat > "$SCRATCH/matrix.yaml" <<'YAML'
entries:
  - name: probe
    cmake_flags: ["-DGLOB=*", "-DSPACE=a b", "-DOTHER=ON"]
  - name: empty
YAML

(cd "$SCRATCH/cwd" && PATH="$SCRATCH/bin:$PATH" IBMW_MATRIX_ROOT="$SCRATCH/root" \
  IBMW_MATRIX_YAML="$SCRATCH/matrix.yaml" \
  bash "$REPO_ROOT/scripts/run_dual_build_matrix.sh" --report --build-only probe empty > "$SCRATCH/out.txt" 2>&1) \
  || fail "matrix harness failed: $(tail -5 "$SCRATCH/out.txt")"

expected=$'-DGLOB=*\n-DSPACE=a b\n-DOTHER=ON'
actual="$(cat "$SCRATCH/args.txt")"
[[ "$actual" == "$expected" ]] || fail "cmake arguments were split or expanded: $actual"
grep -q 'entry=probe  flags=-DGLOB=\* -DSPACE=a b -DOTHER=ON' "$SCRATCH/out.txt" \
  || fail "flags are not shown space separated"
echo "PASS: dual build matrix flags"
