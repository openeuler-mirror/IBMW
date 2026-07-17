// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// R8 validation sample: DELIBERATELY UNANCHORED.
//
// This sample includes iceoryx_ext/iceoryx_transport_driver.h directly
// from a user-region file WITHOUT wrapping the include in a per-rule
// EXEMPT anchor pair. Running
//
//   scripts/check_red_lines.sh
//
// against this file produces a fresh R1 violation, which is the intended
// outcome -- it documents what the un-escape-hatched form looks like and
// pins the check_red_lines scanner against the documented anchor
// convention.
//
// Step T (re)generates scripts/baseline_red_lines.txt with
//
//   --exclude src/samples/cpp/r8_validation/direct_iceoryx_no_anchor_fail/
//
// so the baseline stays at 0 R1 cumulative even with this file present.
// Gated by IBMW_BUILD_R8_FAIL_SAMPLES (default OFF in CI).

#include "iceoryx_ext/iceoryx_transport_driver.h"

int main() {
  return 0;
}
