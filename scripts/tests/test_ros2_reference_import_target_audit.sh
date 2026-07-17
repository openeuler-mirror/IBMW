#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# Synthetic CLI coverage for reference_existing import target_artifact_audit.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IBMW_GEN="$REPO_ROOT/src/tools/ibmw_gen/ibmw_gen.py"

if [[ ! -f "$IBMW_GEN" ]]; then
  echo "FATAL: ibmw-gen not found: $IBMW_GEN" >&2
  exit 1
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

fail() {
  echo "FAIL: $1" >&2
  if [[ $# -gt 1 ]]; then
    echo "-- command output --" >&2
    echo "$2" >&2
  fi
  exit 1
}

make_fake_elf() {
  local path="$1"
  local arch="$2"
  local payload="${3:-}"

  python3 - "$path" "$arch" "$payload" <<'PY'
import pathlib
import sys

path, arch, payload = sys.argv[1:]
machine = {
    "x86": 3,
    "arm": 40,
    "x86_64": 62,
    "aarch64": 183,
}[arch]
data = bytearray(b"\x7fELF")
data.extend(b"\x02\x01\x01")
data.extend(b"\x00" * (18 - len(data)))
data.extend(machine.to_bytes(2, "little"))
data.extend(b"\x00" * 44)
data.extend(payload.encode("utf-8"))
pathlib.Path(path).parent.mkdir(parents=True, exist_ok=True)
pathlib.Path(path).write_bytes(data)
PY
}

make_reference_prefix() {
  local prefix="$1"
  local pkg_dir="$prefix/share/synthetic_srvs"
  mkdir -p "$pkg_dir/srv"
  cat > "$pkg_dir/package.xml" <<'EOF'
<package format="3"><name>synthetic_srvs</name><version>1.0.0</version></package>
EOF
  cat > "$pkg_dir/srv/Ping.srv" <<'EOF'
int32 value
---
int32 doubled
EOF
}

run_expect_fail() {
  local label="$1"
  shift
  local out status

  set +e
  out="$($@ 2>&1)"
  status=$?
  set -e
  if [[ $status -eq 0 ]]; then
    fail "$label unexpectedly passed" "$out"
  fi
  echo "$out"
}

prefix="$SCRATCH/host_ros"
make_reference_prefix "$prefix"

clean_artifact="$SCRATCH/target/libsynthetic_ping.so"
make_fake_elf "$clean_artifact" aarch64

pass_report="$SCRATCH/pass_report.json"
python3 "$IBMW_GEN" import \
  --ros2-prefix "$prefix" \
  --ros2-package synthetic_srvs \
  --ros2-type srv/Ping \
  --artifact-only \
  --output-dir "$SCRATCH/out-pass" \
  --target-artifact "$clean_artifact" \
  --expected-target-arch aarch64 \
  --require-target-audit-pass \
  --format json >"$pass_report"

python3 - "$pass_report" "$clean_artifact" "$prefix" <<'PY'
import json
import pathlib
import sys

report = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
artifact = str(pathlib.Path(sys.argv[2]))
prefix = str(pathlib.Path(sys.argv[3]))
audit = report["target_artifact_audit"]
if audit["status"] != "pass":
    raise SystemExit(f"expected pass audit, got {audit['status']}: {audit['reasons']}")
if audit["expected_arch"] != "aarch64":
    raise SystemExit(f"expected aarch64 audit, got {audit['expected_arch']}")
if prefix not in audit["forbidden_prefixes"]:
    raise SystemExit(f"expected host discovery prefix in forbidden list: {audit['forbidden_prefixes']}")
if audit["artifacts"][0]["path"] != artifact:
    raise SystemExit(f"unexpected artifact path: {audit['artifacts']}")
if audit["artifacts"][0]["arch"] != "aarch64":
    raise SystemExit(f"unexpected artifact arch: {audit['artifacts']}")
PY

leaky_artifact="$SCRATCH/target/libleaky_ping.so"
make_fake_elf "$leaky_artifact" aarch64 "$prefix"

OUT="$(run_expect_fail \
  'reference_existing target audit leak' \
  python3 "$IBMW_GEN" import \
    --ros2-prefix "$prefix" \
    --ros2-package synthetic_srvs \
    --ros2-type srv/Ping \
    --artifact-only \
    --output-dir "$SCRATCH/out-fail" \
    --target-artifact "$leaky_artifact" \
    --expected-target-arch aarch64 \
    --require-target-audit-pass \
    --format json)"
echo "$OUT" | grep -F 'target artifact audit failed' >/dev/null \
  || fail 'leaky artifact failure did not mention target artifact audit failed' "$OUT"
echo "$OUT" | grep -F "$prefix" >/dev/null \
  || fail 'leaky artifact failure did not mention forbidden host prefix' "$OUT"

echo 'PASS: reference_existing import target artifact audit accepts clean synthetic target artifacts and rejects host-prefix leaks.'
