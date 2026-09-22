// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "hz_command.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "base/log_util.h"
#include "discovery_options.h"
#include "engine/topic_io/topic_subscription.h"
#include "rate_statistics.h"

namespace ibmw::cli::topic {
namespace {

using Clock = RateStatistics::Clock;
using ibmw::runtime::core::topic_io::TopicSubscription;
volatile std::sig_atomic_t stop_requested = 0;
void RequestStop(int) { stop_requested = 1; }

class SignalGuard {
 public:
  SignalGuard() {
    stop_requested = 0;
    struct sigaction action {};
    action.sa_handler = RequestStop;
    ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, &interrupt_) != 0) {
      throw std::runtime_error("Cannot install SIGINT handler.");
    }
    if (::sigaction(SIGTERM, &action, &terminate_) != 0) {
      ::sigaction(SIGINT, &interrupt_, nullptr);
      throw std::runtime_error("Cannot install SIGTERM handler.");
    }
  }
  ~SignalGuard() {
    ::sigaction(SIGINT, &interrupt_, nullptr);
    ::sigaction(SIGTERM, &terminate_, nullptr);
  }
 private:
  struct sigaction interrupt_ {}, terminate_ {};
};

class LoggerLevelGuard {
 public:
  LoggerLevelGuard() : previous_(ibmw::common::util::SimpleLogger::GetLogLevel()) {
    ibmw::common::util::SimpleLogger::SetLogLevel(ibmw::common::util::kLogLevelWarn);
  }
  ~LoggerLevelGuard() { ibmw::common::util::SimpleLogger::SetLogLevel(previous_); }
 private:
  uint32_t previous_;
};

std::string Value(const Invocation& invocation, const std::string& name,
                  std::string fallback = {}) {
  const auto it = invocation.options.find(name);
  if (it == invocation.options.end()) return fallback;
  if (it->second.size() != 1 || it->second.front().empty()) {
    throw std::invalid_argument(name + " requires exactly one nonempty value.");
  }
  return it->second.front();
}

double Seconds(const Invocation& invocation, const std::string& name, const char* fallback) {
  const auto value = Value(invocation, name, fallback);
  double number = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (error != std::errc{} || end != value.data() + value.size() ||
      !std::isfinite(number) || number <= 0 || number > 86400 * 365) {
    throw std::invalid_argument(name + " requires a finite positive number of seconds (at most one year).");
  }
  return number;
}

int RunTopicHzCommandImpl(const Invocation& invocation) {
  auto sources = ParseDiscoveryOptions(invocation);
  if (sources.size() != 1) throw std::invalid_argument("topic hz accepts exactly one discovery source.");
  const auto window_text = Value(invocation, "--window", "10000");
  size_t window = 0;
  const auto [end, error] = std::from_chars(window_text.data(), window_text.data() + window_text.size(), window);
  if (error != std::errc{} || end != window_text.data() + window_text.size() || window < 2) {
    throw std::invalid_argument("--window requires an integer of at least 2 messages.");
  }
  const auto timeout = std::chrono::duration<double>(Seconds(invocation, "--timeout", "10"));
  const auto duration = invocation.HasOption("--duration")
      ? std::chrono::duration<double>(Seconds(invocation, "--duration", "1"))
      : std::chrono::duration<double>::max();
  TopicSubscription::Options options{
      .source = sources.front(), .topic = invocation.positional_args.front(),
      .type = Value(invocation, "--type"), .config_file = Value(invocation, "--config-file")};
  if (options.topic.empty()) throw std::invalid_argument("Topic name must not be empty.");
  for (const auto& policy : {std::string("reliability"), std::string("durability")}) {
    const auto option = "--qos-" + policy;
    if (!invocation.HasOption(option)) continue;
    const auto value = Value(invocation, option);
    const bool valid = policy == "reliability"
        ? value == "reliable" || value == "best_effort"
        : value == "volatile" || value == "transient_local";
    if (!valid) throw std::invalid_argument("Invalid value for " + option + ": " + value);
    options.qos.policies[policy] = value;
  }
  if (options.source.backend == ibmw::discovery::DiscoveryBackend::kRegistry &&
      (!options.config_file.empty() || !options.qos.Empty())) {
    throw std::invalid_argument("--config-file and QoS overrides require network observation.");
  }
  if (options.source.backend == ibmw::discovery::DiscoveryBackend::kZenoh && !options.qos.Empty()) {
    throw std::invalid_argument("DDS/ROS 2 QoS overrides do not apply to Zenoh observation.");
  }

  SignalGuard signals;
  LoggerLevelGuard logger_level;
  RateStatistics statistics(window);
  TopicSubscription subscription(std::move(options));
  subscription.Start([&](ibmw::transport::SerializedMessage&&) { statistics.Observe(); });
  const auto discovery_start = Clock::now();
  std::optional<Clock::time_point> measurement_start;
  auto next_refresh = discovery_start;
  auto next_print = discovery_start + std::chrono::seconds(1);
  uint64_t printed_received = 0, reported_dropped = 0;
  auto print = [&] {
    const auto result = statistics.Snapshot();
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
    std::cout.flush();
  };
  while (!stop_requested) {
    const auto now = Clock::now();
    if (now >= next_refresh) {
      if (subscription.Refresh() && !measurement_start) {
        measurement_start = Clock::now();
        std::cerr << "Subscribed to " << invocation.positional_args.front()
                  << "; measuring received rate with a steady clock.\n";
      }
      next_refresh = Clock::now() + std::chrono::milliseconds(250);
      const auto dropped = subscription.DroppedMessages();
      if (dropped != reported_dropped) {
        std::cerr << "ibmw topic hz: Runtime Tap dropped " << dropped << " message(s).\n";
        reported_dropped = dropped;
      }
    }
    if (!measurement_start && now - discovery_start >= timeout) {
      throw std::runtime_error("Timed out waiting for a publisher of '" +
                               invocation.positional_args.front() + "'.");
    }
    if (measurement_start && now - *measurement_start >= duration) break;
    if (now >= next_print) {
      print();
      next_print = now + std::chrono::seconds(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  subscription.Stop();
  if (statistics.Snapshot().received != printed_received) print();
  else if (printed_received == 0) {
    std::cout << "no messages received\n";
  }
  return 0;
}

}  // namespace

void AddHzCommand(Command& topic) {
  using Mode = OptionValueMode;
  auto& hz = topic.AddSubcommand("hz", "Measure the received topic rate using a steady clock");
  hz.SetHandler(RunTopicHzCommandImpl);
  hz.AddArgument("<topic>", "Topic to observe (including hidden topics)");
  AddDiscoveryOptions(hz, false);
  hz.AddOption("--window", "-w", "messages", "Window size in messages, at least 2 (default: 10000)", Mode::kRequired);
  hz.AddOption("--type", {}, "type", "Select a message type when the topic has multiple types", Mode::kRequired);
  hz.AddOption("--timeout", {}, "seconds", "Wait for a publisher (default: 10 seconds)", Mode::kRequired);
  hz.AddOption("--duration", {}, "seconds", "Stop after this observation duration (default: until Ctrl+C)", Mode::kRequired);
  hz.AddOption("--config-file", {}, "path", "Transport connection configuration; domains come from discovery options", Mode::kRequired);
  hz.AddOption("--qos-reliability", {}, "policy", "best_effort (default; reliable for DDS exclusive ownership) or reliable; DDS/ROS 2 only", Mode::kRequired);
  hz.AddOption("--qos-durability", {}, "policy", "volatile (default) or transient_local; DDS/ROS 2 only", Mode::kRequired);
  hz.AddOption("--include-hidden-topics", {}, {}, "Explicit topic queries already include hidden topics");
}

}  // namespace ibmw::cli::topic
