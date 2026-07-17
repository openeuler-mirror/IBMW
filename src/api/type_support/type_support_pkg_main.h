// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/util/type_support_base.h"
#include "type_support/type_support_pkg_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief get length of the type support array
 *
 * @return length of the type support array
 */
IBMW_TYPE_SUPPORT_PKG_EXPORT size_t IbmwDynlibGetTypeSupportArrayLength();

/**
 * @brief get the type support array
 *
 * @return type support array
 */
IBMW_TYPE_SUPPORT_PKG_EXPORT const ibmw_type_support_base_t** IbmwDynlibGetTypeSupportArray();

#ifdef __cplusplus
}
#endif
