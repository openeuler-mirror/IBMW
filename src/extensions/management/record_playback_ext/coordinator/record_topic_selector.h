// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <set>
#include <string>
#include <vector>

#include "record_playback_ext/coordinator/bag_capture_planner.h"

namespace ibmw::extensions::record_playback_extension {

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

  std::set<std::string> Select(const CaptureGraph& graph) const;

 private:
  Options options_;
};

}  // namespace ibmw::extensions::record_playback_extension
