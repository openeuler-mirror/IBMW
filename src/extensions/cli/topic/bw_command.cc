// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "bw_command.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "bandwidth_statistics.h"
#include "observation_command.h"

namespace ibmw::cli::topic {
namespace {

constexpr size_t kDefaultBwWindow = 100;

int RunTopicBwCommandImpl(const Invocation& invocation) {
  auto options = ParseObservationOptions(invocation, "bw", kDefaultBwWindow);
  BandwidthStatistics statistics(options.window);
  return RunObservation(std::move(options),
      [&](ibmw::transport::SerializedMessage&& message) { statistics.Observe(message.PayloadSize()); },
      [&](bool final) {
        const auto result = statistics.Snapshot();
        if (result.received == 0) {
          std::cout << (final ? "no messages received\n" : "no new messages\n");
        } else if (!result.ready) {
          std::cout << "waiting for enough messages to calculate bandwidth\n";
        } else {
          const double scale = result.bytes_per_second < 1000 ? 1
                             : result.bytes_per_second < 1000000 ? 1000 : 1000000;
          const char* unit = scale == 1 ? "B" : scale == 1000 ? "KB" : "MB";
          std::ostringstream output;
          output << std::fixed << std::setprecision(scale == 1 ? 0 : 2)
                 << result.bytes_per_second / scale << ' ' << unit << "/s from "
                 << result.window << " messages\n\tMessage size mean: "
                 << result.mean / scale << ' ' << unit << " min: "
                 << result.minimum / scale << ' ' << unit << " max: "
                 << result.maximum / scale << ' ' << unit << '\n';
          std::cout << output.str();
        }
      });
}

}  // namespace

void AddBwCommand(Command& topic) {
  auto& bw = topic.AddSubcommand("bw", "Measure received serialized payload bandwidth (excludes transport overhead)");
  bw.SetHandler(RunTopicBwCommandImpl);
  AddObservationOptions(bw, kDefaultBwWindow);
}

}  // namespace ibmw::cli::topic
