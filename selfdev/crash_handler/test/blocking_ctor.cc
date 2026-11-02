// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Test-only library for CH-33: its constructor runs with the dynamic
// loader's lock held (dlopen), reports that it started on the fd named by
// IBMW_CRASH_TEST_CTOR_FD, then never returns.

#include <unistd.h>

#include <cstdlib>

__attribute__((constructor)) static void BlockInConstructor() {
  const char* fd = std::getenv("IBMW_CRASH_TEST_CTOR_FD");
  if (fd != nullptr) {
    const char c = 1;
    ssize_t r = write(std::atoi(fd), &c, 1);
    (void)r;
  }
  for (;;) pause();
}
