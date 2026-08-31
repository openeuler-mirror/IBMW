// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <set>
#include <string>
#include <vector>

#include "engine/runtime_graph/topic_graph.h"
#include "engine/runtime_graph/runtime_registration.h"

namespace ibmw::extensions::record_playback_extension {

using ibmw::runtime::core::runtime_graph::TopicGraph;
using ibmw::runtime::core::runtime_graph::TopicAdvertisement;
using ibmw::runtime::core::runtime_graph::RuntimeRegistration;

class RecordTopicSelector {
 public:
  struct Options {
    std::vector<std::string> explicit_topics;
    bool all = false;
    std::string regex;
    std::string exclude_regex;
    bool include_hidden_topics = false;
    bool include_unpublished_topics = false;
    bool ignore_leaf_topics = false;
  };

  explicit RecordTopicSelector(Options options);

  std::set<std::string> Select(const TopicGraph& graph) const;

 private:
  Options options_;
};

}  // namespace ibmw::extensions::record_playback_extension
