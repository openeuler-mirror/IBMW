// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "hz_command.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "observation_command.h"
#include "rate_statistics.h"

namespace ibmw::cli::topic {
namespace {

constexpr size_t kDefaultHzWindow = 10000;

int RunTopicHzCommandImpl(const Invocation& invocation) {
  auto options = ParseObservationOptions(invocation, "hz", kDefaultHzWindow);
  RateStatistics statistics(options.window);
  uint64_t printed_received = 0;
  return RunObservation(std::move(options),
      [&](ibmw::transport::SerializedMessage&&) { statistics.Observe(); },
      [&](bool final) {
        const auto result = statistics.Snapshot();
        if (final && result.received == printed_received) {
          if (result.received == 0) std::cout << "no messages received\n";
          return;
        }
        if (result.received == printed_received) {
          std::cout << "no new messages\n";
        } else if (result.window < 2 || result.rate == 0) {
          std::cout << "waiting for enough messages to calculate rate\n";
        } else {
          std::ostringstream output;
          output << std::fixed << std::setprecision(3) << "average rate: " << result.rate
                 << "\n\tmin: " << result.minimum << "s max: " << result.maximum
                 << "s std dev: " << std::setprecision(5) << result.deviation
                 << "s window: " << result.window << '\n';
          std::cout << output.str();
        }
        printed_received = result.received;
      });
}

}  // namespace

void AddHzCommand(Command& topic) {
  auto& hz = topic.AddSubcommand("hz", "Measure the received topic rate using a steady clock");
  hz.SetHandler(RunTopicHzCommandImpl);
  AddObservationOptions(hz, kDefaultHzWindow);
}

}  // namespace ibmw::cli::topic
