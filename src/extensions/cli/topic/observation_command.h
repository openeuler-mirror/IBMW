// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "cli/command.h"
#include "engine/topic_io/topic_subscription.h"

namespace ibmw::cli::topic {

struct ObservationOptions {
  std::string command;
  ibmw::runtime::core::topic_io::TopicSubscription::Options subscription;
  size_t window;
  std::chrono::duration<double> timeout;
  std::chrono::duration<double> duration;
};

std::string ObservationValue(const Invocation& invocation, const std::string& name,
                             std::string fallback = {});
void AddObservationOptions(ibmw::cli::Command& command,
                           std::optional<size_t> default_window = std::nullopt);
ObservationOptions ParseObservationOptions(
    const Invocation& invocation, std::string_view command,
    std::optional<size_t> default_window = std::nullopt);
// Observe runs on subscription callbacks. Print runs on the calling thread;
// its final invocation follows Stop(), after all subscription callbacks drain.
// Poll runs every iteration on the calling thread and may request early completion.
int RunObservation(ObservationOptions options,
                   ibmw::transport::SerializedMessageCallback observe,
                   const std::function<void(bool final)>& print = {},
                   const std::function<bool()>& poll = {});

}  // namespace ibmw::cli::topic
