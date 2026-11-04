// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// R8 validation sample: DELIBERATELY UNANCHORED (shm_ext).
//
// shm_ext counterpart of direct_iceoryx_no_anchor_fail. This sample includes
// shm_ext/shm_transport_driver.h directly from a user-region file WITHOUT
// wrapping the include in a per-rule EXEMPT anchor pair. Running
//
//   scripts/check_red_lines.sh
//
// against this file produces a fresh R1 violation, which is the intended
// outcome -- it pins the check_red_lines scanner against the documented
// anchor convention for the shm transport as well.
//
// scripts/baseline_red_lines.txt is generated with
//
//   --exclude src/samples/cpp/r8_validation/direct_shm_no_anchor_fail/
//
// so the baseline does not absorb this deliberate hit.
// Gated by IBMW_BUILD_R8_FAIL_SAMPLES (default OFF in CI).

#include "shm_ext/shm_transport_driver.h"

int main() {
  return 0;
}
