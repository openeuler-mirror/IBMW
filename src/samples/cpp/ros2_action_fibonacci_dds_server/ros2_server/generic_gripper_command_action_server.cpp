// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <control_msgs/action/gripper_command.hpp>

#include "cpp/action/action_server_node.h"
#include "gripper_command_action_server_binding.h"

namespace {

using GripperCommand = control_msgs::action::GripperCommand;
using Traits = ibmw::action::ActionTraits<GripperCommand>;
using Hook = ibmw::samples::ros2_action_fibonacci::GripperCommandHook;

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
  return "/gripper_command";
}

uint32_t ParseDomainId(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], "--domain-id") == 0) {
      return static_cast<uint32_t>(std::stoul(argv[i + 1]));
    }
  }
  const char* domain = std::getenv("ROS_DOMAIN_ID");
  if (domain == nullptr || std::strlen(domain) == 0U) {
    return 0;
  }
  return static_cast<uint32_t>(std::stoul(domain));
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  ibmw::action::ActionServerNode<Traits, Hook> server(ParseActionName(argc, argv));
  if (!server.Start(ParseDomainId(argc, argv))) {
    return 1;
  }
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return 0;
}
