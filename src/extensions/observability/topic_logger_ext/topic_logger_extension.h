// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>

#include "engine/extension/extension_base.h"
#include "engine/engine.h"

namespace ibmw::extensions::topic_logger_extension {
class TopicLoggerExtension : public ExtensionBase {
 public:
  struct Options {};

 public:
  TopicLoggerExtension();
  ~TopicLoggerExtension() override = default;

  std::string_view Name() const noexcept override { return "topic_logger_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterTopicLoggerDriver();

 private:
  std::vector<std::function<void()>> post_init_transport_hook_task_vec_;
  std::vector<std::function<void()>> post_start_transport_hook_task_vec_;
  std::vector<std::function<void()>> pre_shutdown_transport_hook_task_vec_;
  std::vector<std::function<void()>> pre_start_hook_task_vec_;

  Options options_;

  std::atomic_bool stop_flag_ = false;
};

}  // namespace ibmw::extensions::topic_logger_extension
