// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <future>
#include "cpp/context/context.h"
#include "cpp/module_base.h"

namespace ibmw::examples::cpp::context::logger_module {

class LoggerModule : public ibmw::Module {
 public:
  LoggerModule() = default;
  ~LoggerModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "ContextLoggerModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  std::shared_ptr<ibmw::context::Context> ctx_ptr_;
  ibmw::executor::ExecutorHandle executor_;
};

}  // namespace ibmw::examples::cpp::context::logger_module
