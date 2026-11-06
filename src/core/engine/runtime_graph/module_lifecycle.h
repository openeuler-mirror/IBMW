// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "nlohmann/json.hpp"
#include "extension_api/ibmw_module_lifecycle.h"

namespace ibmw::runtime::core::runtime_graph {

nlohmann::json ModuleLifecycleStatusToJson(
    const ibmw::extension::ModuleLifecycleStatus& status);
ibmw::extension::ModuleLifecycleStatus ModuleLifecycleStatusFromJson(
    const nlohmann::json& value);

}  // namespace ibmw::runtime::core::runtime_graph
