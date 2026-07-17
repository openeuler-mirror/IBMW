// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/service/service_driver.h"

#include "rclcpp/service.hpp"

namespace ibmw::extensions::ros2_extension {

class Ros2AdapterServer : public rclcpp::ServiceBase {
 public:
  Ros2AdapterServer(const std::shared_ptr<rcl_node_t>& node_handle,
                    const runtime::core::service::ServiceFuncWrapper& service_func_wrapper,
                    const std::string& real_ros2_func_name,
                    const rclcpp::QoS& qos);
  ~Ros2AdapterServer() override = default;

  std::shared_ptr<void> create_request() override;
  std::shared_ptr<rmw_request_id_t> create_request_header() override;
  void handle_request(std::shared_ptr<rmw_request_id_t> request_header,
                      std::shared_ptr<void> request) override;

  void Start() { run_flag_.store(true); }
  void Shutdown() { run_flag_.store(false); }

 private:
  std::atomic_bool run_flag_ = false;
  const runtime::core::service::ServiceFuncWrapper& service_func_wrapper_;
  std::string real_ros2_func_name_;
};

class Ros2AdapterWrapperServer : public rclcpp::ServiceBase {
 public:
  Ros2AdapterWrapperServer(const std::shared_ptr<rcl_node_t>& node_handle,
                           const runtime::core::service::ServiceFuncWrapper& service_func_wrapper,
                           const std::string& real_ros2_func_name,
                           const rclcpp::QoS& qos);
  ~Ros2AdapterWrapperServer() override = default;

  std::shared_ptr<void> create_request() override;
  std::shared_ptr<rmw_request_id_t> create_request_header() override;
  void handle_request(std::shared_ptr<rmw_request_id_t> request_header,
                      std::shared_ptr<void> request) override;

  void Start() { run_flag_.store(true); }
  void Shutdown() { run_flag_.store(false); }

 private:
  void ReturnRspWithStatusCode(const std::shared_ptr<rmw_request_id_t>& request_header, uint32_t code);

 private:
  std::atomic_bool run_flag_ = false;
  const runtime::core::service::ServiceFuncWrapper& service_func_wrapper_;
  std::string real_ros2_func_name_;
};

}  // namespace ibmw::extensions::ros2_extension
