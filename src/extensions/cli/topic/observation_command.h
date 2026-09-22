// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <functional>
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

void AddObservationOptions(ibmw::cli::Command& command, size_t default_window);
ObservationOptions ParseObservationOptions(
    const Invocation& invocation, std::string_view command, size_t default_window);
// Observe runs on subscription callbacks. Print runs on the calling thread;
// its final invocation follows Stop(), after all subscription callbacks drain.
int RunObservation(ObservationOptions options,
                   ibmw::transport::SerializedMessageCallback observe,
                   const std::function<void(bool final)>& print);

}  // namespace ibmw::cli::topic
