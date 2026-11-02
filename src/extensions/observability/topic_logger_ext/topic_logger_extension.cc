// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "topic_logger_ext/topic_logger_extension.h"
#include "topic_logger_ext/global.h"
#include "topic_logger_ext/topic_logger_driver.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::topic_logger_extension::TopicLoggerExtension::Options> {
  using Options = ibmw::extensions::topic_logger_extension::TopicLoggerExtension::Options;

  static Node encode(const Options& rhs) {
    Node node;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::topic_logger_extension {

TopicLoggerExtension::TopicLoggerExtension() {
  SetExtensionLogger(&g_logger);
}

bool TopicLoggerExtension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kLogger),
                          [this] {
                            RegisterTopicLoggerDriver();
                          });

  core_ptr_->RegisterHook(runtime::core::After(runtime::core::Phase::kInitialize,
                                                runtime::core::Subsystem::kTransport),
                          [this] {for (const auto& task : post_init_transport_hook_task_vec_) task(); });

  core_ptr_->RegisterHook(runtime::core::BeforePhase(runtime::core::Phase::kStart),
                          [this] {for (const auto& task : pre_start_hook_task_vec_) task(); });

  core_ptr_->RegisterHook(runtime::core::After(runtime::core::Phase::kStart,
                                                runtime::core::Subsystem::kTransport),
                          [this] {for (const auto& task : post_start_transport_hook_task_vec_) task(); });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kStopping,
                                                  runtime::core::Subsystem::kTransport),
                          [this] { for (const auto& task : pre_shutdown_transport_hook_task_vec_) task(); });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void TopicLoggerExtension::OnShutdown() {
  stop_flag_ = true;
}

void TopicLoggerExtension::RegisterTopicLoggerDriver() {
  namespace core = runtime::core;
  core_ptr_->GetLoggingSubsystem()
      .RegisterLoggerDriverGenFunc(
          "topic_logger",
          [this]() -> std::unique_ptr<core::logger::LoggerDriverBase> {
            auto topic_logger_driver_ptr = std::make_unique<TopicLoggerDriver>();

            // register core
            topic_logger_driver_ptr->RegisterGetSenderRefFunc(
                [this](std::string_view topic_name) -> ibmw::transport::SenderRef {
                  return ibmw::transport::TransportRef(core_ptr_->GetTransportBus().GetBusAccess().NativeHandle()).GetPublisher(topic_name);
                });

            // register get_executor_func
            topic_logger_driver_ptr->RegisterGetExecutorFunc(
                [this](std::string_view executor_name) -> ibmw::executor::ExecutorHandle {
                  return core_ptr_->GetExecutorBus().GetExecutor(executor_name);
                });

            // register publisher
            post_init_transport_hook_task_vec_.emplace_back(
                [topic_logger_driver = topic_logger_driver_ptr.get()]() {
                  topic_logger_driver->RegisterLogPublisher();
                });

            // set max_msg_size
            pre_start_hook_task_vec_.emplace_back(
                [topic_logger_driver = topic_logger_driver_ptr.get()]() {
                  topic_logger_driver->SetMaxMsgSize();
                });

            // startup publisher
            post_start_transport_hook_task_vec_.emplace_back(
                [topic_logger_driver = topic_logger_driver_ptr.get()]() {
                  topic_logger_driver->StartupPublisher();
                });

            // stop publisher
            pre_shutdown_transport_hook_task_vec_.emplace_back(
                [topic_logger_driver = topic_logger_driver_ptr.get()]() {
                  topic_logger_driver->StopPublisher();
                });

            return topic_logger_driver_ptr;
          });
}

}  // namespace ibmw::extensions::topic_logger_extension

IBMW_EXT_ENTRY(ibmw::extensions::topic_logger_extension::TopicLoggerExtension)
