// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "shm_ext/global.h"

IBMW_DEFINE_EXTENSION_LOGGER(ibmw::extensions::shm_extension)

namespace ibmw::extensions::shm_extension {
void SetShmExtensionLogger(::ibmw::logger::LoggerRef logger) { g_logger.Set(logger); }
}  // namespace ibmw::extensions::shm_extension
