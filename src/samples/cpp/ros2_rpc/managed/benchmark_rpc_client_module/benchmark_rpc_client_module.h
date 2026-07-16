// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>

#include "cpp/co/async_scope.h"
#include "cpp/co/task.h"
#include "cpp/module_base.h"

#include "RosTestRpc.ibmw_rpc.srv.h"

namespace ibmw::examples::cpp::ros2_rpc::benchmark_rpc_client_module {

class BenchmarkRpcClientModule : public ibmw::Module {
 public:
  BenchmarkRpcClientModule() = default;
  ~BenchmarkRpcClientModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "BenchmarkRpcClientModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  auto GetLogger() { return core_.GetLogger(); }

  co::Task<void> MainLoop();

  co::Task<void> WaitForServiceServer();

  struct BenchPlan {
    enum class PerfMod : uint8_t {
      kBench,
      kFixedFreq
    };

    PerfMod mode = PerfMod::kBench;

    uint32_t freq;
    uint32_t msg_size;
    uint32_t parallel;
    uint32_t msg_count;
  };

  co::Task<void> StartSinglePlan(uint32_t plan_id, BenchPlan plan);
  co::Task<void> StartFixedFreqPlan(uint32_t parallel_id, BenchPlan plan, std::vector<uint32_t>& perf_data);
  co::Task<void> StartBenchPlan(uint32_t parallel_id, BenchPlan plan, std::vector<uint32_t>& perf_data);

 private:
  ibmw::Runtime core_;

  co::AsyncScope scope_;
  std::atomic_bool run_flag_ = true;

  std::shared_ptr<example_ros2::srv::RosTestRpcCoAccess> access_;

  ibmw::executor::ExecutorHandle client_statistics_executor_;  // name: client_statistics_executor
  std::vector<ibmw::executor::ExecutorHandle> executor_vec_;   // name: client_executor_x

  // cfg
  uint32_t max_parallel_;
  std::vector<BenchPlan> bench_plans_;
  std::string service_name_;
};

}  // namespace ibmw::examples::cpp::ros2_rpc::benchmark_rpc_client_module
