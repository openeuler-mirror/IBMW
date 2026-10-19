// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "extension_api/ibmw_runtime_graph.h"
#include "nlohmann/json.hpp"

namespace ibmw::runtime::core::runtime_graph {

using ibmw::extension::TopicAdvertisement;
using ibmw::extension::TransportCaptureCapability;

nlohmann::json TopicAdvertisementToJson(
    const TopicAdvertisement& advertisement);
TopicAdvertisement TopicAdvertisementFromJson(const nlohmann::json& value);

}  // namespace ibmw::runtime::core::runtime_graph
