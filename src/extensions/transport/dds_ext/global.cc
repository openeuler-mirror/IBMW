// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "dds_ext/global.h"

IBMW_DEFINE_EXTENSION_LOGGER(ibmw::extensions::dds_extension)

namespace ibmw::extensions::dds_extension {

static DdsTransportDriver* global_dds_driver = nullptr;

void SetDdsTransportDriver(DdsTransportDriver* driver) { global_dds_driver = driver; }
DdsTransportDriver* GetDdsTransportDriver() { return global_dds_driver; }

}  // namespace ibmw::extensions::dds_extension
