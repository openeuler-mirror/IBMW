#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# Synthetic test harness for the R8 (strict) and R12 (report-only) red-line
# detectors in scripts/check_red_lines.sh.
#
# This harness creates three minimal C++ source files in a scratch directory,
# invokes check_red_lines.sh with --scan-path pointing at that directory, and
# asserts that:
#   1) case_r8_violation.cc  -> R8 fires (LoanedSample<> + SenderAccess<>::Send(
#                                outside any IBMW_HAS_* guard)
#   2) case_r8_guarded.cc    -> R8 does NOT fire (same code, but inside
#                                #if IBMW_HAS_ICEORYX ... #endif)
#   3) case_r12_runtime.cc   -> R12 fires (IBMW_HAS_DDS used as a runtime
#                                expression, not a #if/#ifdef/defined() context)
#
# The harness is intended to be invoked both directly and via the pytest wrapper
# src/tools/ibmw_gen/tests/test_red_lines_runner.py (Gate 1).
#
# Test conventions are documented in docs/contributing/testing.md.
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

# ── case 1: R8 violation — naked LoanedSample<> + SenderAccess<>::Send( ─────
cat > "$SCRATCH/case_r8_violation.cc" <<'EOF'
// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "fake.h"
namespace ibmw::synthetic_r8_violation {
void run() {
  ibmw::transport::LoanedSample<int> loan;
  ibmw::transport::SenderAccess<int>::Send(loan);
}
}  // namespace ibmw::synthetic_r8_violation
EOF

# ── case 2: R8 guarded — same code wrapped in #if IBMW_HAS_ICEORYX ──────────
cat > "$SCRATCH/case_r8_guarded.cc" <<'EOF'
// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "fake.h"
namespace ibmw::synthetic_r8_guarded {
void run() {
#if IBMW_HAS_ICEORYX
  ibmw::transport::LoanedSample<int> loan;
  ibmw::transport::SenderAccess<int>::Send(loan);
#endif
}
}  // namespace ibmw::synthetic_r8_guarded
EOF

# ── case 3: R12 — IBMW_HAS_DDS used as a runtime expression ─────────────────
cat > "$SCRATCH/case_r12_runtime.cc" <<'EOF'
// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
namespace ibmw::synthetic_r12 {
int probe() {
  if (IBMW_HAS_DDS) {
    return 1;
  }
  return 0;
}
}  // namespace ibmw::synthetic_r12
EOF

# ── invoke checker ─────────────────────────────────────────────────────────
OUT="$(bash "$CHECKER" --report --scan-path "$SCRATCH" 2>&1 || true)"

fail() { echo "FAIL: $1" >&2; echo "── checker output ──" >&2; echo "$OUT" >&2; exit 1; }

# Assertion 1: R8 must fire on case_r8_violation.cc
echo "$OUT" | grep -E "^R8: .*case_r8_violation\.cc:" >/dev/null \
  || fail "R8 did not fire on case_r8_violation.cc"

# Assertion 2: R8 must NOT fire on case_r8_guarded.cc
if echo "$OUT" | grep -E "^R8: .*case_r8_guarded\.cc:" >/dev/null; then
  fail "R8 incorrectly fired on case_r8_guarded.cc (guarded by IBMW_HAS_ICEORYX)"
fi

# Assertion 3: R12 must fire on case_r12_runtime.cc
echo "$OUT" | grep -E "^R12: .*case_r12_runtime\.cc:" >/dev/null \
  || fail "R12 did not fire on case_r12_runtime.cc"

# Assertion 4: R12 must NOT fire on case_r8_guarded.cc (uses #if, compile-time)
if echo "$OUT" | grep -E "^R12: .*case_r8_guarded\.cc:" >/dev/null; then
  fail "R12 incorrectly fired on case_r8_guarded.cc (#if context is compile-time)"
fi

echo "PASS: R8 fired on violation, suppressed by guard; R12 fired on runtime use."
exit 0
