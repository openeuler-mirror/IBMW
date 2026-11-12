// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include "topic_check.h"

namespace ibmw::cli::doctor {

// One policy observation, not a claim that an entire connection works.
struct QosCheckResult {
  discovery::DiscoverySource source;
  std::string driver;
  std::string topic;
  std::string type;
  std::string publisher;
  std::string subscriber;
  std::string policy;
  std::string offered;
  std::string requested;
  CheckStatus status = CheckStatus::kSkipped;
  std::string message;
};

std::vector<QosCheckResult> CheckQos(
    const discovery::DiscoverySnapshot& snapshot, bool include_hidden = false);

}  // namespace ibmw::cli::doctor
