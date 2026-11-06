// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "extension_api/ibmw_discovery.h"

namespace ibmw::cli::doctor {

enum class CheckStatus { kOk, kWarning, kUnknown, kSkipped };

struct TopicCheckResult {
  discovery::DiscoverySource source;
  std::string topic;
  std::string type;
  size_t publishers = 0;
  size_t subscribers = 0;
  CheckStatus status = CheckStatus::kSkipped;
  std::string message;
};

// Checks observed endpoint presence only, not QoS or data delivery. Sources,
// domains, observer configurations, logical/wire names and types stay separate.
std::vector<TopicCheckResult> CheckTopics(
    const discovery::DiscoverySnapshot& snapshot, bool include_hidden = false);

}  // namespace ibmw::cli::doctor
