// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::util {

std::string CheckYamlNodes(
    YAML::Node standard_node, YAML::Node checked_node, const std::string& path, uint32_t level = 0);

// Expands ${ENV} in every scalar (keys and values) of an already parsed tree,
// so an environment value can never add YAML structure (CORE-020).
void ReplaceEnvVarsInScalars(YAML::Node node);

}  // namespace ibmw::runtime::core::util
