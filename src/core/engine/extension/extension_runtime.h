// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "extension_api/ibmw_extension_runtime.h"

namespace ibmw::runtime::core {
class Engine;
}

namespace ibmw::runtime::core::extension {

std::unique_ptr<ibmw::extension::RuntimeV1> MakeExtensionRuntimeV1(
    Engine& engine);

}  // namespace ibmw::runtime::core::extension
