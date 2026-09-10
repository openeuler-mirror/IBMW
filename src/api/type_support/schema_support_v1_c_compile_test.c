// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "c/util/schema_support_base.h"

_Static_assert(offsetof(ibmw_schema_support_v1_t, abi_version) == 0, "ABI");
_Static_assert(offsetof(ibmw_schema_support_v1_t, type_name) == 8, "ABI");
_Static_assert(offsetof(ibmw_schema_support_package_v1_t, entries) == 16,
               "ABI");
_Static_assert(IBMW_SCHEMA_SUPPORT_V1_REQUIRED_SIZE ==
                   offsetof(ibmw_schema_support_v1_t, reserved),
               "ABI");
_Static_assert(IBMW_SCHEMA_SUPPORT_PACKAGE_V1_REQUIRED_SIZE ==
                   offsetof(ibmw_schema_support_package_v1_t, reserved),
               "ABI");

int ibmw_schema_support_v1_c_header_compile_test(void) {
  ibmw_schema_support_package_v1_t package = {
      .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
      .struct_size = sizeof(ibmw_schema_support_package_v1_t)};
  return package.abi_version == IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1 ? 0 : 1;
}
