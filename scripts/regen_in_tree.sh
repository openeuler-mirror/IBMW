#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# In-tree regen of all ibmw-gen sample projects.
# For every src/samples/cpp/<demo>/<demo>.ibmw.yaml file, runs ibmw_gen.py
# with -o = <sample_dir>/<project.name_with_gen_suffix>, then reports
# PASS / FAIL counts. ibmw-template/my_ibmw_project is regenerated from
# ibmw-template/ibmw_template.ibmw.yaml the same way.
#
# Used by the verification workflow to regenerate all 14 in-tree samples.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

PASS=0
FAIL=0
FAIL_LIST=()

# Private log directory: a fixed /tmp name could be a planted link or be
# overwritten by a concurrent run. Kept when a sample fails.
LOG_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ibmw_regen.XXXXXX") || exit 2
trap '[[ $FAIL -eq 0 ]] && rm -rf -- "$LOG_DIR"' EXIT

shopt -s nullglob
yaml_files=(src/samples/cpp/*/*.ibmw.yaml)
shopt -u nullglob

if [[ ${#yaml_files[@]} -eq 0 ]]; then
    echo "ERROR: no .ibmw.yaml found under src/samples/cpp/*/" >&2
    exit 2
fi

for y in "${yaml_files[@]}" ibmw-template/ibmw_template.ibmw.yaml; do
    sdir=$(dirname "$y")
    name=$(python3 -c "import sys, yaml; print(yaml.safe_load(open(sys.argv[1]))['project']['name'])" "$y")
    # Append _gen suffix only if not already present
    if [[ "$y" == ibmw-template/* ]]; then
        out="$sdir/$name"
    elif [[ "$name" == *_gen ]]; then
        out="$sdir/$name"
    else
        out="$sdir/${name}_gen"
    fi
    log="$LOG_DIR/regen_$(basename "$y" .ibmw.yaml).log"
    if python3 src/tools/ibmw_gen/ibmw_gen.py "$y" -o "$out" --force > "$log" 2>&1; then
        PASS=$((PASS+1))
        n_new=$(grep -oE '[0-9]+ new'    "$log" | head -1)
        n_ov=$(grep -oE '[0-9]+ overwrite' "$log" | head -1)
        echo "PASS: $(basename "$y")  ($n_new, $n_ov) -> $out"
    else
        FAIL=$((FAIL+1))
        FAIL_LIST+=("$y")
        echo "FAIL: $y  (log: $log)"
    fi
done

echo
echo "===== SUMMARY ====="
echo "PASS: $PASS"
echo "FAIL: $FAIL"
if [[ $FAIL -gt 0 ]]; then
    for f in "${FAIL_LIST[@]}"; do
        echo "  - $f"
    done
    exit 1
fi
exit 0
