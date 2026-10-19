// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <thread>

#include <optional>

#include "engine/extension/extension_base.h"

#include "rclcpp/executors.hpp"
#include "rclcpp/rclcpp.hpp"

namespace ibmw::extensions::ros2_extension {

class Ros2Extension : public ExtensionBase {
 public:
  struct Options {
    std::string node_name;
    std::optional<uint32_t> domain_id;
    // SingleThreaded | StaticSingleThreaded | MultiThreaded
    std::string executor_type = "MultiThreaded";
    uint32_t executor_thread_num = 2;
    std::string executor_name = "ros2_executor";
    std::string executor_sched_policy;
    std::vector<uint32_t> executor_bind_cpus;
    bool auto_initialize_logging = true;
  };

 public:
  Ros2Extension();
  ~Ros2Extension() override = default;

  std::string_view Name() const noexcept override { return "ros2_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterRos2RpcDriver();
  void RegisterRos2TransportDriver();
  void SetExecutorThreadSchedOption();

 private:
  Options options_;

  std::shared_ptr<rclcpp::Context> ros2_context_;
  std::shared_ptr<rclcpp::Node> ros2_node_ptr_;
  std::shared_ptr<rclcpp::Executor> ros2_node_executor_ptr_;
  std::unique_ptr<std::thread> ros2_thread_ptr_;
};

}  // namespace ibmw::extensions::ros2_extension
