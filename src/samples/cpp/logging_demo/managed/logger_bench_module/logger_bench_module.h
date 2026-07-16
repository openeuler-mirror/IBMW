// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once
#include <atomic>
#include <future>
#include "cpp/module_base.h"

namespace ibmw::examples::cpp::logger::logger_bench_module {
class LoggerBenchModule : public ibmw::Module {
 public:
  LoggerBenchModule() = default;
  ~LoggerBenchModule() override = default;
  ModuleDescriptor Info() const override { return ModuleDescriptor{.module_name = "LoggerBenchModule"}; }
  bool Setup(ibmw::Runtime ibmw_ptr) override;
  bool Run() override;
  void Stop() override;
 private:
  auto GetLogger() { return core_.GetLogger(); }
 private:
  ibmw::Runtime core_;
  std::vector<uint32_t> log_data_size_vec_;
  uint32_t log_bench_num_ = 10000;
  std::atomic_bool run_flag_ = false;
  std::promise<void> log_loop_stop_sig_;
};
}
