// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "extension_api/ibmw_extension_base.h"
#include "extension_api/ibmw_extension_export.h"

/// Descriptor returned by the single extension entry point.
struct ibmw_ext_entry_t {
  ibmw::EngineExtensionBase* (*create)();
  void (*destroy)(const ibmw::EngineExtensionBase*);
};

extern "C" {
IBMW_CORE_EXTENSION_EXPORT ibmw_ext_entry_t IbmwExtEntry();
}
