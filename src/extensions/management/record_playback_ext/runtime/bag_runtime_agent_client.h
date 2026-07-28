// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include "nlohmann/json.hpp"
#include "record_playback_ext/runtime/bag_runtime_agent.h"

namespace ibmw::extensions::record_playback_extension {

class BagRuntimeAgentClient {
 public:
  explicit BagRuntimeAgentClient(std::string control_socket)
      : control_socket_(std::move(control_socket)) {}

  std::vector<TopicAdvertisement> ListTopics() const;
  TopicAdvertisement GetTopicDescriptor(
      std::string_view topic,
      std::string_view publisher_id = {}) const;
  TapSessionDescriptor StartTap(
      std::string_view topic,
      std::string_view publisher_id = {}) const;
  bool StopTap(std::string_view session_id) const;

 private:
  nlohmann::json Call(
      std::string_view method,
      nlohmann::json params = nlohmann::json::object()) const;

  std::string control_socket_;
};

}  // namespace ibmw::extensions::record_playback_extension
