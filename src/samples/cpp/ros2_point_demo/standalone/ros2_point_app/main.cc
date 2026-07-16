// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include <csignal>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"
#include "../../managed/point_publisher_module/point_publisher_module.h"
#include "../../managed/point_subscriber_module/point_subscriber_module.h"

using namespace ibmw::runtime::core;
using namespace ibmw::examples::ros2_point_demo;

Engine* global_core_ptr = nullptr;

void SignalHandler(int sig) {
  if (global_core_ptr && (sig == SIGINT || sig == SIGTERM)) {
    global_core_ptr->Shutdown();
    return;
  }
  raise(sig);
}

int main(int argc, char** argv) {
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  std::cout << "========================================\n";
  std::cout << "ROS2 Point Demo (Plain Type Zero-Copy)\n";
  std::cout << "========================================\n";

  try {
    Engine core;
    global_core_ptr = &core;

    // Register modules
    PointPublisherModule publisher_module;
    PointSubscriberModule subscriber_module;

    core.GetModuleBus().RegisterModule(publisher_module.NativeHandle());
    core.GetModuleBus().RegisterModule(subscriber_module.NativeHandle());

    // Initialize
    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config_file_path>\n";
      std::cerr << "Example: " << argv[0] << " ./cfg/ros2_point_cfg.yaml\n";
      return -1;
    }

    core.Initialize(options);

    // Start
    std::cout << "Starting ROS2 Point Demo...\n";
    std::cout << "Publisher: Publishing geometry_msgs::msg::Point (Plain type)\n";
    std::cout << "Subscriber: Listening for Point messages\n";
    std::cout << "\nPress Ctrl+C to exit\n";
    std::cout << "========================================\n";

    core.Start();

    // Shutdown
    core.Shutdown();

    global_core_ptr = nullptr;

  } catch (const std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return -1;
  }

  std::cout << "\nROS2 Point Demo exit.\n";
  return 0;
}
