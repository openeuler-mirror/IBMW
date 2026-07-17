// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// R12 validation sample: DELIBERATELY UNCOMPILABLE.
//
// This file deliberately uses a compile-time IBMW_HAS_<TRANSPORT>
// macro as a *runtime* expression (inside an `if (...)`), which is
// forbidden by strict R12. Those macros are
// compile-time selectors — abusing them at runtime defeats the
// link-time elision they exist to enable.
//
// Running
//
//   scripts/check_red_lines.sh
//
// against this file produces a fresh R12 violation, which is the
// intended outcome — it documents what the un-guarded form looks like
// and pins the check_red_lines scanner against the documented
// compile-time-only convention.
//
// This file is INTENTIONALLY NOT added to any CMakeLists.txt. The dual
// build matrix entry `r12_runtime_use_fail` invokes the red-line
// scanner via `--scan-path` and asserts the scanner exits non-zero,
// proving the R12 detector still fires on a fresh runtime-use
// violation.
//
// Production checks exclude this intentional negative fixture so it does not
// affect the normal red-line baseline. See docs/contributing/testing.md.

#include "cpp/features.h"

int main() {
  // R12: runtime use of compile-time IBMW_HAS_* macro.
  if (IBMW_HAS_DDS) {
    return 0;
  }
  return 1;
}
