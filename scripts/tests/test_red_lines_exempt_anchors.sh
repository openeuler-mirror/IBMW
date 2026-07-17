#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# Synthetic test harness for the per-rule IBMW_R<N>_EXEMPT_BEGIN/END anchor
# mechanism in scripts/check_red_lines.sh.
#
# Asserts:
#   1) case_anchored.h     -> R1 fires on the unanchored include only;
#                              the anchored include is suppressed AND the
#                              anchor markers do not themselves emit any
#                              violation (rule-scoped, original line numbers
#                              preserved for baseline diff stability).
#   2) case_unanchored.h   -> R1 fires on both includes (control).
#   3) case_r1_only.h      -> R1 anchor does NOT suppress R2 (rule-scoped);
#                              the namespace alias still triggers R2 even
#                              though it sits below an R1 anchor.
#   4) case_unmatched.h    -> stray IBMW_R1_EXEMPT_BEGIN with no END leaves
#                              the rest of the file in 'in_exempt=1' state
#                              for that file only; this is documented behavior
#                              (mirrors R7's awk-stateful semantics) and the
#                              suppression must NOT bleed across files.
#
# The harness is intended to be invoked both directly and via the pytest
# wrapper src/tools/ibmw_gen/tests/test_red_lines_runner.py (Gate 1).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CHECKER="$REPO_ROOT/scripts/check_red_lines.sh"

if [[ ! -x "$CHECKER" ]]; then
  echo "FATAL: checker not found or not executable: $CHECKER" >&2
  exit 1
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT
mkdir -p "$SCRATCH/managed/case_anchored" \
         "$SCRATCH/managed/case_unanchored" \
         "$SCRATCH/managed/case_r1_only" \
         "$SCRATCH/managed/case_unmatched"

cat > "$SCRATCH/managed/case_anchored/case_anchored.h" <<'EOF'
// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once
// IBMW_R1_EXEMPT_BEGIN: anchor self-test
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END
#include "dds_ext/dds_transport_driver.h"
EOF

cat > "$SCRATCH/managed/case_unanchored/case_unanchored.h" <<'EOF'
// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once
#include "iceoryx_ext/iceoryx_transport_driver.h"
#include "dds_ext/dds_transport_driver.h"
EOF

cat > "$SCRATCH/managed/case_r1_only/case_r1_only.h" <<'EOF'
// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once
// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END
namespace iox_ext = ibmw::extensions::iceoryx_extension;
EOF

cat > "$SCRATCH/managed/case_unmatched/case_unmatched.h" <<'EOF'
// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once
// IBMW_R1_EXEMPT_BEGIN: stray begin without end (documented behavior)
#include "iceoryx_ext/iceoryx_transport_driver.h"
EOF

OUT="$(bash "$CHECKER" --report --scan-path "$SCRATCH" 2>&1 || true)"

fail() { echo "FAIL: $1" >&2; echo "── checker output ──" >&2; echo "$OUT" >&2; exit 1; }

echo "$OUT" | grep -E "^R1: .*case_anchored\.h:6:.*dds_ext/dds_transport_driver" >/dev/null \
  || fail "R1 did not fire on case_anchored.h:6 (unanchored dds include)"

if echo "$OUT" | grep -E "^R1: .*case_anchored\.h:4:" >/dev/null; then
  fail "R1 incorrectly fired on case_anchored.h:4 (line is inside IBMW_R1_EXEMPT_BEGIN/END)"
fi

if echo "$OUT" | grep -E "^R[0-9]+: .*case_anchored\.h:(3|5):" >/dev/null; then
  fail "anchor marker line itself was reported as a violation"
fi

echo "$OUT" | grep -E "^R1: .*case_unanchored\.h:3:" >/dev/null \
  || fail "control: R1 did not fire on case_unanchored.h:3"
echo "$OUT" | grep -E "^R1: .*case_unanchored\.h:4:" >/dev/null \
  || fail "control: R1 did not fire on case_unanchored.h:4"

echo "$OUT" | grep -E "^R2: .*case_r1_only\.h:6:" >/dev/null \
  || fail "R2 did not fire on case_r1_only.h:6 (R1 anchor must not suppress R2)"

if echo "$OUT" | grep -E "^R1: .*case_unmatched\.h:" >/dev/null; then
  fail "R1 unexpectedly fired on case_unmatched.h (stray BEGIN should leave file in exempt state)"
fi

if echo "$OUT" | grep -E "^R1: .*case_anchored\.h:6:" | grep -E ":(7|8|9|10):" >/dev/null; then
  fail "case_anchored.h R1 hit reported wrong line number (anchor mechanism must preserve original NR)"
fi

echo "PASS: per-rule IBMW_R<N>_EXEMPT anchors correctly scope, preserve line numbers, and isolate per-file."
exit 0
