// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/util/schema_support_base.h"
#include "c/util/type_support_base.h"
#include <stddef.h>
#include "type_support/type_support_pkg_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1 UINT32_C(1)
#define IBMW_TYPE_SUPPORT_PACKAGE_V1_RESERVED_COUNT 8u

/**
 * @brief Versioned descriptor exported by every TypeSupport package.
 *
 * The descriptor is the sole discovery entry point.  The first two fields
 * are an append-only ABI prefix; consumers must validate struct_size before
 * reading fields beyond that prefix.  All entries and the optional schema
 * descriptor remain owned by the exporting DSO until it is unloaded.
 */
typedef struct ibmw_type_support_package_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  size_t entry_count;
  const ibmw_type_support_base_t** entries;
  const ibmw_schema_support_package_v1_t* schema_support;
  uint64_t reserved[IBMW_TYPE_SUPPORT_PACKAGE_V1_RESERVED_COUNT];
} ibmw_type_support_package_v1_t;

#define IBMW_TYPE_SUPPORT_PACKAGE_V1_REQUIRED_SIZE \
  offsetof(ibmw_type_support_package_v1_t, reserved)

/**
 * @brief Return the immutable V1 TypeSupport package descriptor.
 */
IBMW_TYPE_SUPPORT_PKG_EXPORT const ibmw_type_support_package_v1_t*
IbmwDynlibGetTypeSupportPackageV1();

#ifdef __cplusplus
}
#endif
