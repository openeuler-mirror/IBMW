// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "extension_api/extension_logger.h"

IBMW_DECLARE_EXTENSION_LOGGER(ibmw::extensions::dds_extension)

namespace ibmw::extensions::dds_extension {

// Forward declaration
class DdsTransportDriver;

// Global accessor for DDS driver (set by extension during initialization)
__attribute__((visibility("default"))) void SetDdsTransportDriver(DdsTransportDriver* driver);
__attribute__((visibility("default"))) DdsTransportDriver* GetDdsTransportDriver();

}  // namespace ibmw::extensions::dds_extension

#undef MW_LOGGER
#define MW_LOGGER ibmw::extensions::dds_extension::g_logger.Get()
#include "base/log_util.h"
