// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <map>
#include <string>

#include "engine/transport/topic_descriptor.h"
#include "nlohmann/json.hpp"

namespace ibmw::extensions::record_playback_extension {

struct TransportCaptureCapability {
  uint32_t abi_version = 0;
  std::string mode;
  uint32_t priority = 100;
  std::string extension_name;
  std::string extension_path;
  std::string extension_options_yaml;
  std::string driver_options_yaml;
  std::string provider_instance_option;
  std::string provider_clear_option;
  std::string provider_strip_leading_slash_option;
};

struct TopicAdvertisement {
  std::string runtime_id;
  std::string control_socket;
  std::string publisher_id;
  ibmw::runtime::core::transport::TopicDescriptor topic;
  std::string type_support_package;
  std::map<std::string, TransportCaptureCapability> capture_capabilities;
  bool published = true;
  bool embedded_tap = false;
  bool hidden = false;
};

nlohmann::json TopicAdvertisementToJson(
    const TopicAdvertisement& advertisement);
TopicAdvertisement TopicAdvertisementFromJson(const nlohmann::json& value);

}  // namespace ibmw::extensions::record_playback_extension
