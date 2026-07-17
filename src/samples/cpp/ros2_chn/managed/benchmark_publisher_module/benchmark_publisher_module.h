// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <future>

#include "cpp/module_base.h"

namespace ibmw::examples::cpp::ros2_chn::benchmark_publisher_module {

class BenchmarkPublisherModule : public ibmw::Module {
 public:
  BenchmarkPublisherModule() = default;
  ~BenchmarkPublisherModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "BenchmarkPublisherModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  void MainLoop();

  struct BenchPlan {
    enum class PerfMod : uint8_t {
      kMultiTopic,
      kParallel
    };

    PerfMod mode = PerfMod::kMultiTopic;
    uint32_t transport_frq;
    uint32_t msg_size;
    uint32_t topic_number = 1;
    uint32_t parallel_number = 1;
    uint32_t msg_count;
  };

  void StartSinglePlan(uint32_t plan_id, BenchPlan plan);
  void StartMultiTopicPlan(uint32_t plan_id, BenchPlan plan);
  void StartParallelPlan(uint32_t plan_id, BenchPlan plan);

 private:
  ibmw::Runtime core_;

  std::atomic_bool run_flag_ = false;
  std::promise<void> stop_sig_;

  ibmw::executor::ExecutorHandle publish_control_executor_;  // name: publish_control_executor
  ibmw::transport::SenderRef signal_publisher_;          // topic name: benchmark_signal

  std::vector<ibmw::executor::ExecutorHandle> executor_vec_;
  std::vector<ibmw::transport::SenderRef> publisher_vec_;

  // cfg
  uint32_t max_topic_number_;
  uint32_t max_parallel_number_;
  std::vector<BenchPlan> bench_plans_;
};

}  // namespace ibmw::examples::cpp::ros2_chn::benchmark_publisher_module
