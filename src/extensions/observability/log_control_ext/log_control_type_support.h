// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <string_view>
#include "c/util/type_support_base.h"
#include "log_control_ext/log_control_types.h"
#include "type_support/type_support_pkg_export.h"

namespace ibmw::extensions::log_control_extension {
// Shared protocol support for the server and CLI. JSON names/layout stay stable;
// builds with DDS additionally attach native DDS/CDR support to the same types.
IBMW_TYPE_SUPPORT_PKG_EXPORT const ibmw_type_support_base_t* FindLogControlTypeSupport(std::string_view name);
template <typename T>
const ibmw_type_support_base_t* GetLogControlTypeSupport() {
  return FindLogControlTypeSupport(T::kTypeName);
}
}  // namespace ibmw::extensions::log_control_extension
