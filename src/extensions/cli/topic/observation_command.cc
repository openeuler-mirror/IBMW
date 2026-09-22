// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "observation_command.h"

#include <charconv>
#include <cmath>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "discovery_options.h"
#include "base/log_util.h"
#include "engine/topic_io/topic_subscription.h"

namespace ibmw::cli::topic {
namespace {
using Clock = std::chrono::steady_clock;
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

}  // namespace

ObservationOptions ParseObservationOptions(
    const Invocation& invocation, std::string_view command, size_t default_window) {
  auto sources = ParseDiscoveryOptions(invocation);
  if (sources.size() != 1) throw std::invalid_argument("topic " + std::string(command) + " accepts exactly one discovery source.");
  const auto window_text = Value(invocation, "--window", std::to_string(default_window));
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

  return {.command = std::string(command), .subscription = std::move(options),
          .window = window, .timeout = timeout, .duration = duration};
}

int RunObservation(ObservationOptions options,
                   ibmw::transport::SerializedMessageCallback observe,
                   const std::function<void(bool final)>& print) {
  SignalGuard signals;
  LoggerLevelGuard logger_level;
  const auto topic = options.subscription.topic;
  TopicSubscription subscription(std::move(options.subscription));
  subscription.Start(std::move(observe));
  const auto discovery_start = Clock::now();
  std::optional<Clock::time_point> measurement_start;
  auto next_refresh = discovery_start;
  auto next_print = discovery_start + std::chrono::seconds(1);
  uint64_t reported_dropped = 0;
  while (!stop_requested) {
    auto now = Clock::now();
    if (now >= next_refresh) {
      if (subscription.Refresh() && !measurement_start) {
        measurement_start = Clock::now();
        std::cerr << "Subscribed to " << topic << "; using a steady clock.\n";
      }
      next_refresh = Clock::now() + std::chrono::milliseconds(250);
      const auto dropped = subscription.DroppedMessages();
      if (dropped != reported_dropped) {
        std::cerr << "ibmw topic " << options.command << ": Runtime Tap dropped "
                  << dropped << " message(s).\n";
        reported_dropped = dropped;
      }
      now = Clock::now();
    }
    if (!measurement_start && now - discovery_start >= options.timeout) {
      throw std::runtime_error("Timed out waiting for a publisher of '" + topic + "'.");
    }
    if (measurement_start && now - *measurement_start >= options.duration) break;
    if (now >= next_print) {
      print(false);
      std::cout.flush();
      next_print = now + std::chrono::seconds(1);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  subscription.Stop();
  print(true);
  std::cout.flush();
  return 0;
}

void AddObservationOptions(ibmw::cli::Command& command, size_t default_window) {
  command.AddArgument("<topic>", "Topic to observe (including hidden topics)");
  AddDiscoveryOptions(command, false);
  using Mode = ibmw::cli::OptionValueMode;
  command.AddOption("--window", "-w", "messages", "Window size in messages, at least 2 (default: " + std::to_string(default_window) + ")", Mode::kRequired);
  command.AddOption("--type", {}, "type", "Select a message type when the topic has multiple types", Mode::kRequired);
  command.AddOption("--timeout", {}, "seconds", "Wait for a publisher (default: 10 seconds)", Mode::kRequired);
  command.AddOption("--duration", {}, "seconds", "Stop after this observation duration (default: until Ctrl+C)", Mode::kRequired);
  command.AddOption("--config-file", {}, "path", "Transport connection configuration; domains come from discovery options", Mode::kRequired);
  command.AddOption("--qos-reliability", {}, "policy", "best_effort (default; reliable for DDS exclusive ownership) or reliable; DDS/ROS 2 only", Mode::kRequired);
  command.AddOption("--qos-durability", {}, "policy", "volatile (default) or transient_local; DDS/ROS 2 only", Mode::kRequired);
  command.AddOption("--include-hidden-topics", {}, {}, "Explicit topic queries already include hidden topics");
}

}  // namespace ibmw::cli::topic
