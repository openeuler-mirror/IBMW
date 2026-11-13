// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "extension_api/ibmw_reporter_v1.h"

#if defined(__cplusplus)
static_assert(offsetof(ibmw_reporter_v1, abi_version) == 0, "ABI");
static_assert(offsetof(ibmw_reporter_v1, owner) == 8, "ABI");
#else
_Static_assert(offsetof(ibmw_reporter_v1, abi_version) == 0, "ABI");
_Static_assert(offsetof(ibmw_reporter_v1, owner) == 8, "ABI");
#endif

int ibmw_reporter_v1_c_header_compile_test(void) {
  const ibmw_reporter_v1* reporter = ibmw_reporter_query_v1();
  return reporter == 0 ? 0 : (int)reporter->abi_version;
}
