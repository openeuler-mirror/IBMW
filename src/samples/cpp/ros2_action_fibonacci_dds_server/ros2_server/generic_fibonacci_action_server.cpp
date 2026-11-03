// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

#include <example_interfaces/action/fibonacci.hpp>

#include "cpp/action/action_server_node.h"
#include "fibonacci_action_server_binding.h"

namespace {

using Fibonacci = example_interfaces::action::Fibonacci;
using Traits = ibmw::action::ActionTraits<Fibonacci>;
using Hook = ibmw::samples::ros2_action_fibonacci::FibonacciHook;

std::atomic_bool g_stop{false};

void HandleSignal(int) {
  g_stop.store(true);
}

std::string ParseActionName(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--action-name") == 0) {
      return argv[i + 1];
    }
  }
  return "/fibonacci";
}

constexpr uint32_t kMaxDomainId = 232;

// Parses the whole of `text` as a base-10 integer in [min_value, max_value].
bool ParseInteger(const char* text, uint32_t min_value, uint32_t max_value, uint32_t& out) {
  if (text == nullptr || *text == '\0') {
    return false;
  }
  const char* end = text + std::strlen(text);
  uint32_t value = 0;
  const auto [ptr, ec] = std::from_chars(text, end, value);
  if (ec != std::errc() || ptr != end || value < min_value || value > max_value) {
    return false;
  }
  out = value;
  return true;
}

std::optional<uint32_t> ParseDomainId(int argc, char** argv) {
  const char* flag = "--domain-id";
  const char* text = nullptr;
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      text = argv[i + 1];
      break;
    }
  }
  if (text == nullptr) {
    flag = "ROS_DOMAIN_ID";
    text = std::getenv("ROS_DOMAIN_ID");
    if (text == nullptr || std::strlen(text) == 0U) {
      return 0U;
    }
  }
  uint32_t domain_id = 0;
  if (!ParseInteger(text, 0, kMaxDomainId, domain_id)) {
    std::cerr << "[FAIL] Invalid value for " << flag << ": '" << text << "'" << std::endl;
    return std::nullopt;
  }
  return domain_id;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  const auto domain_id = ParseDomainId(argc, argv);
  if (!domain_id.has_value()) {
    return 2;
  }
  ibmw::action::ActionServerNode<Traits, Hook> server(ParseActionName(argc, argv));
  if (!server.Start(*domain_id)) {
    return 1;
  }
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return 0;
}
