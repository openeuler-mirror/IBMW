// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Macros that replace the per-extension global.h / global.cc boilerplate.
//
// In global.h:
//   #pragma once
//   #include "extension_api/extension_logger.h"
//   IBMW_DECLARE_EXTENSION_LOGGER(ibmw::extensions::my_extension)
//   #undef MW_LOGGER
//   #define MW_LOGGER ibmw::extensions::my_extension::g_logger.Get()
//   #include "base/log_util.h"
//
// In global.cc:
//   #include "<extension>/global.h"
//   IBMW_DEFINE_EXTENSION_LOGGER(ibmw::extensions::my_extension)

#pragma once

#include "engine/extension/extension_base.h"

/// Declare the extension's global ExtensionLogger as extern in the given namespace.
#define IBMW_DECLARE_EXTENSION_LOGGER(ns) \
  namespace ns { extern ::ibmw::ExtensionLogger g_logger; }

/// Define (allocate storage for) the extension's global ExtensionLogger.
#define IBMW_DEFINE_EXTENSION_LOGGER(ns) \
  namespace ns { ::ibmw::ExtensionLogger g_logger; }
