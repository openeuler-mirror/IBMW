// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "extension_api/extension_logger.h"

IBMW_DECLARE_EXTENSION_LOGGER(ibmw::extensions::shm_extension)

namespace ibmw::extensions::shm_extension {
// Installs the plugin's logger for a host that links the plugin and uses
// ShmTransportDriver without the engine (ibmw_shm_ext_bench_so); the engine
// sets it through the extension's logger hook instead. A function, not the
// exported g_logger object, so the ExtensionLogger layout stays private.
__attribute__((visibility("default"))) void SetShmExtensionLogger(::ibmw::logger::LoggerRef logger);
}  // namespace ibmw::extensions::shm_extension

#undef MW_LOGGER
#define MW_LOGGER ibmw::extensions::shm_extension::g_logger.Get()
#include "base/log_util.h"
