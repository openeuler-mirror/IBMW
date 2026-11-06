// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "engine/runtime_graph/runtime_graph_agent.h"

namespace ibmw::runtime::core::runtime_graph {

class RuntimeGraphAgentClient {
 public:
  explicit RuntimeGraphAgentClient(std::string control_socket)
      : control_socket_(std::move(control_socket)) {}

  std::vector<TopicAdvertisement> ListTopics() const;
  // Live read-only snapshot; unsupported agents report an error, not an empty list.
  std::vector<ibmw::extension::ModuleLifecycleStatus> ListModuleLifecycle() const;
  TopicAdvertisement GetTopicDescriptor(
      std::string_view topic,
      std::string_view publisher_id = {}) const;
  TapSessionDescriptor StartTap(
      std::string_view topic,
      std::string_view publisher_id = {}) const;
  bool StopTap(std::string_view session_id) const;
  TapSessionDescriptor StartPlayback(
      std::string_view topic,
      std::string_view subscriber_id = {}) const;
  bool StopPlayback(std::string_view session_id) const;

 private:
  nlohmann::json Call(
      std::string_view method,
      nlohmann::json params = nlohmann::json::object()) const;

  std::string control_socket_;
};

}  // namespace ibmw::runtime::core::runtime_graph
