// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/runtime_graph/runtime_registration.h"

namespace ibmw::runtime::core::runtime_graph {

class TopicGraph {
 public:
  void Clear();
  void MergeRuntime(const RuntimeRegistration& runtime);
  void ReplaceRuntimes(const std::vector<RuntimeRegistration>& runtimes);

  std::vector<TopicAdvertisement> Find(std::string_view topic_name) const;
  std::vector<std::string> Topics() const;

 private:
  std::unordered_map<std::string, std::vector<TopicAdvertisement>> topics_;
};

}  // namespace ibmw::runtime::core::runtime_graph
