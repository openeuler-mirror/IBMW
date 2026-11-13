// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Test-only library for CH-39..CH-41: dlopen'ed by the test after Install(),
// like an IBMW package loaded after the logger subsystem installed the
// handler, so it is missing from the module table until RefreshModules().

#include <cstdio>

volatile int g_late_recurse_limit = 1 << 30;  // never reached; keeps -Winfinite-recursion quiet

extern "C" __attribute__((noinline, visibility("default"))) int CrashTestLateRecurse(int depth) {
  volatile char pad[1024];
  pad[0] = static_cast<char>(depth);
  if (depth >= g_late_recurse_limit) return pad[0];
  return CrashTestLateRecurse(depth + 1) + pad[0];
}

// A plain null store with junk words on the stack, so [sp] holds a value
// that looks like nothing (0x1111): an ordinary data fault must not report a
// "caller" read from [sp]. Not instrumented: under ASan/UBSan the store would
// be reported instead of raising a real SIGSEGV.
extern "C" __attribute__((noinline, visibility("default"), no_sanitize("address", "undefined")))
void CrashTestLateNullWrite() {
  volatile long junk[8] = {0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888};
  char buf[64];
  std::snprintf(buf, sizeof buf, "%ld", junk[3]);
  volatile int* p = reinterpret_cast<volatile int*>(junk[0] - 0x1111);
  *p = buf[0];
}
