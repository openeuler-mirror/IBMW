// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "extension_api/extension_logger.h"

IBMW_DECLARE_EXTENSION_LOGGER(ibmw::extensions::mqtt_extension)

#undef MW_LOGGER
#define MW_LOGGER ibmw::extensions::mqtt_extension::g_logger.Get()
#include "base/log_util.h"
