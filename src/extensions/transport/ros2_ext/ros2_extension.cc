// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_ext/ros2_extension.h"
#include "engine/engine.h"
#include "engine/thread_tools.h"
#include "irobot_lock_free_events_queue/lock_free_events_queue.hpp"
#include "rcl/logging.h"
#include "rclcpp/version.h"
#if RCLCPP_VERSION_MAJOR == 16
  // Humble: use irobot events_executor
  #include "rclcpp/executors/events_executor/events_executor.hpp"
#elif RCLCPP_VERSION_MAJOR == 28
  // Jazzy: use system events_executor
  #include "rclcpp/experimental/executors/events_executor/events_executor.hpp"
#endif
#include "ros2_ext/global.h"
#include "ros2_ext/ros2_transport_driver.h"
#include "ros2_ext/ros2_rpc_driver.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::ros2_extension::Ros2Extension::Options> {
  using Options = ibmw::extensions::ros2_extension::Ros2Extension::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["node_name"] = rhs.node_name;
    if (rhs.domain_id) node["domain_id"] = *rhs.domain_id;
    node["executor_type"] = rhs.executor_type;
    if (rhs.executor_type == "MultiThreaded") {
      node["executor_thread_num"] = rhs.executor_thread_num;
    }
    node["executor_name"] = rhs.executor_name;
    node["executor_sched_policy"] = rhs.executor_sched_policy;
    node["executor_bind_cpus"] = rhs.executor_bind_cpus;
    node["auto_initialize_logging"] = rhs.auto_initialize_logging;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    rhs.node_name = node["node_name"].as<std::string>();
    if (node["domain_id"]) rhs.domain_id = node["domain_id"].as<uint32_t>();

    if (node["executor_type"])
      rhs.executor_type = node["executor_type"].as<std::string>();

    if (rhs.executor_type == "MultiThreaded" && node["executor_thread_num"])
      rhs.executor_thread_num = node["executor_thread_num"].as<uint32_t>();

    if (node["auto_initialize_logging"]) {
      rhs.auto_initialize_logging = node["auto_initialize_logging"].as<bool>();
    }

    if (node["executor_name"])
      rhs.executor_name = node["executor_name"].as<std::string>();

    if (node["executor_sched_policy"])
      rhs.executor_sched_policy = node["executor_sched_policy"].as<std::string>();

    if (node["executor_bind_cpus"])
      rhs.executor_bind_cpus = node["executor_bind_cpus"].as<std::vector<uint32_t>>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::ros2_extension {

Ros2Extension::Ros2Extension() {
  SetExtensionLogger(&g_logger);
}

bool Ros2Extension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }

  rclcpp::InitOptions op;

  op.auto_initialize_logging(options_.auto_initialize_logging);
  op.shutdown_on_signal = false;

  if (options_.domain_id) op.set_domain_id(*options_.domain_id);
  ros2_context_ = std::make_shared<rclcpp::Context>();
  ros2_context_->init(0, nullptr, op);
  ros2_node_ptr_ = std::make_shared<rclcpp::Node>(
      options_.node_name, rclcpp::NodeOptions().context(ros2_context_));
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = ros2_context_;

  if (options_.executor_type == "SingleThreaded") {
    ros2_node_executor_ptr_ =
        std::make_shared<rclcpp::executors::SingleThreadedExecutor>(executor_options);
  } else if (options_.executor_type == "StaticSingleThreaded") {
    ros2_node_executor_ptr_ =
        std::make_shared<rclcpp::executors::StaticSingleThreadedExecutor>(executor_options);
  } else if (options_.executor_type == "MultiThreaded") {
    ros2_node_executor_ptr_ =
        std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
            executor_options, options_.executor_thread_num);
  } else if (options_.executor_type == "EventsExecutor") {
#if RCLCPP_VERSION_MAJOR == 16
    // Humble: use irobot events_executor
    ros2_node_executor_ptr_ =
        std::make_shared<rclcpp::executors::EventsExecutor>(
            std::make_unique<LockFreeEventsQueue>(), false, executor_options);
#elif RCLCPP_VERSION_MAJOR == 28
    // Jazzy: use system events_executor in experimental namespace
    ros2_node_executor_ptr_ =
        std::make_shared<rclcpp::experimental::executors::EventsExecutor>(
            std::make_unique<LockFreeEventsQueue>(), false, executor_options);
#endif
  } else {
    MW_THROW("Invalid ros2 executor_type '{}'.",
                      options_.executor_type);
  }

  ros2_node_executor_ptr_->add_node(ros2_node_ptr_);

  core_ptr->RegisterHook(runtime::core::AfterPhase(runtime::core::Phase::kStart), [this] {
    ros2_thread_ptr_ = std::make_unique<std::thread>(
        [this]() {
          SetExecutorThreadSchedOption();
          ros2_node_executor_ptr_->spin();
        });
  });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                  runtime::core::Subsystem::kService),
                          [this] { RegisterRos2RpcDriver(); });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                  runtime::core::Subsystem::kTransport),
                          [this] { RegisterRos2TransportDriver(); });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void Ros2Extension::OnShutdown() {
  if (ros2_node_executor_ptr_) {
    ros2_node_executor_ptr_->cancel();
  }
  if (ros2_context_ && ros2_context_->is_valid()) {
    ros2_context_->shutdown("ROS2 extension shutdown");
  }

  if (ros2_thread_ptr_ && ros2_thread_ptr_->joinable()) {
    ros2_thread_ptr_->join();
  }
  ros2_thread_ptr_.reset();
  ros2_node_executor_ptr_.reset();
  ros2_node_ptr_.reset();
  ros2_context_.reset();
}

void Ros2Extension::SetExecutorThreadSchedOption() {
  try {
    ibmw::runtime::core::util::SetNameForCurrentThread(options_.executor_name);
    ibmw::runtime::core::util::BindCpuForCurrentThread(options_.executor_bind_cpus);
    ibmw::runtime::core::util::SetCpuSchedForCurrentThread(options_.executor_sched_policy);
  } catch (const ibmw::common::util::IbmwException& e) {
    MW_WARN("Set thread policy for ros2 executor '{}' get exception, {}",
               Name(), e.what());
  }
}

void Ros2Extension::RegisterRos2RpcDriver() {
  std::unique_ptr<runtime::core::service::ServiceDriver> ros2_rpc_driver_ptr =
      std::make_unique<Ros2RpcDriver>();

  static_cast<Ros2RpcDriver*>(ros2_rpc_driver_ptr.get())
      ->RegisterGetExecutorFunc(
          [this](std::string_view executor_name) -> ibmw::executor::ExecutorHandle {
            return core_ptr_->GetExecutorBus().GetExecutor(executor_name);
          });

  static_cast<Ros2RpcDriver*>(ros2_rpc_driver_ptr.get())
      ->SetNodePtr(ros2_node_ptr_);

  core_ptr_->GetServiceDispatcher().RegisterRpcDriver(
      std::move(ros2_rpc_driver_ptr));
}

void Ros2Extension::RegisterRos2TransportDriver() {
  std::unique_ptr<runtime::core::transport::TransportDriver> ros2_transport_driver_ptr =
      std::make_unique<Ros2TransportDriver>();

  static_cast<Ros2TransportDriver*>(ros2_transport_driver_ptr.get())
      ->SetNodePtr(ros2_node_ptr_);

  const auto operations = static_cast<Ros2TransportDriver*>(ros2_transport_driver_ptr.get())->SerializedOperations();
  core_ptr_->GetTransportBus().RegisterTransportDriver(
      std::move(ros2_transport_driver_ptr), {},
      runtime::core::transport::SerializedTransportCapabilityV2{
          .capture_mode = runtime::core::transport::SerializedCaptureMode::kNativeSerialized,
          .publish_mode = runtime::core::transport::SerializedPublishMode::kBus,
          .serialization = runtime::core::transport::SerializedFormatSupport::kNative,
          .schema = runtime::core::transport::SerializedSchemaSupport::kTypeSupportPackage,
          .loan = runtime::core::transport::SerializedLoanSupport::kSharedPayload,
          .callback_thread = runtime::core::transport::SerializedCallbackThread::kTransportManaged,
          .capture_priority = 4,
          .provider_name = Name(),
          .provider_instance_option = "node_name",
          .operations = operations});
}

}  // namespace ibmw::extensions::ros2_extension

IBMW_EXT_ENTRY(ibmw::extensions::ros2_extension::Ros2Extension)
