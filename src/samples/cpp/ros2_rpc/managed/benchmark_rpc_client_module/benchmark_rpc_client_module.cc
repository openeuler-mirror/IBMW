// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "benchmark_rpc_client_module/benchmark_rpc_client_module.h"

#include <algorithm>
#include <chrono>
#include <numeric>

#include "cpp/co/ibmw_context.h"
#include "cpp/co/inline_executor.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::ros2_rpc::benchmark_rpc_client_module {

std::vector<uint8_t> GenerateRandomString(int min_length, int max_length) {
  static constexpr std::string_view kChars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  srand(time(nullptr));

  int length = rand() % (max_length - min_length + 1) + min_length;
  std::vector<uint8_t> result;
  result.reserve(length);

  for (int i = 0; i < length; ++i) {
    // Convert characters directly to uint8_t (ASCII value)
    result.push_back(static_cast<uint8_t>(kChars[rand() % kChars.length()]));
  }

  return result;
}

std::vector<uint8_t> GenerateRandomString(int length) {
  return GenerateRandomString(length, length);
}

bool BenchmarkRpcClientModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    // Read cfg
    auto file_path = core_.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      max_parallel_ = cfg_node["max_parallel"].as<uint32_t>();

      if (cfg_node["service_name"]) {
        service_name_ = cfg_node["service_name"].as<std::string>();
      }

      if (cfg_node["bench_plans"] && cfg_node["bench_plans"].IsSequence()) {
        for (const auto& bench_plan_node : cfg_node["bench_plans"]) {
          BenchPlan bench_plan;

          auto perf_mode = bench_plan_node["perf_mode"].as<std::string>();
          if (perf_mode == "fixed-freq") {
            bench_plan.mode = BenchPlan::PerfMod::kFixedFreq;
          } else if (perf_mode == "bench") {
            bench_plan.mode = BenchPlan::PerfMod::kBench;
          } else {
            throw ibmw::common::util::IbmwException("Unsupport perf mode: " + perf_mode);
          }

          if (bench_plan.mode == BenchPlan::PerfMod::kFixedFreq) {
            bench_plan.freq = bench_plan_node["freq"].as<uint32_t>();
          }

          bench_plan.msg_size = bench_plan_node["msg_size"].as<uint32_t>();
          bench_plan.parallel = bench_plan_node["parallel"].as<uint32_t>();
          bench_plan.msg_count = bench_plan_node["msg_count"].as<uint32_t>();

          MW_REQUIRE(
              bench_plan.parallel <= max_parallel_,
              "Bench plan parallel({}) is greater than max parallel({})",
              bench_plan.parallel, max_parallel_);

          bench_plans_.emplace_back(bench_plan);
        }
      }
    }

    // Get service handle
    auto rpc_handle = core_.GetService();
    MW_REQUIRE(rpc_handle, "Get service handle failed.");

    // Register rpc client
    bool ret = false;
    if (service_name_.empty()) {
      ret = example_ros2::srv::RegisterRosTestRpcClientFunc(rpc_handle);
    } else {
      ret = example_ros2::srv::RegisterRosTestRpcClientFunc(rpc_handle, service_name_);
    }

    MW_REQUIRE(ret, "Register client failed.");

    // Create rpc access
    access_ = std::make_shared<example_ros2::srv::RosTestRpcCoAccess>(rpc_handle);

    if (!service_name_.empty()) {
      access_->SetServiceName(service_name_);
    }

    // Check executor
    client_statistics_executor_ = core_.GetExecutor().GetExecutor("client_statistics_executor");
    MW_REQUIRE(
        client_statistics_executor_ && client_statistics_executor_.SupportsTimedDispatch(),
        "Get executor 'client_statistics_executor' failed.");

    for (uint32_t ii = 0; ii < max_parallel_; ++ii) {
      auto executor_name = "client_executor_" + std::to_string(ii);
      auto executor = core_.GetExecutor().GetExecutor(executor_name);
      MW_REQUIRE(
          executor && executor.SupportsTimedDispatch(),
          "Get executor '{}' failed.", executor_name);

      executor_vec_.emplace_back(executor);
    }

  } catch (const std::exception& e) {
    MW_ERROR("Init failed, {}", e.what());
    return false;
  }

  MW_INFO("Setup succeeded.");

  return true;
}

bool BenchmarkRpcClientModule::Run() {
  try {
    scope_.spawn(co::On(co::InlineExecutor(), MainLoop()));
  } catch (const std::exception& e) {
    MW_ERROR("Start failed, {}", e.what());
    return false;
  }

  MW_INFO("Run succeeded.");
  return true;
}

void BenchmarkRpcClientModule::Stop() {
  try {
    run_flag_ = false;
    co::SyncWait(scope_.complete());
  } catch (const std::exception& e) {
    MW_ERROR("Shutdown failed, {}", e.what());
    return;
  }

  MW_INFO("Stop succeeded.");
}

co::Task<void> BenchmarkRpcClientModule::MainLoop() {
  try {
    MW_INFO("Start Bench.");

    auto statistics_executor = co::IbmwExecutor(client_statistics_executor_);
    co_await co::Schedule(statistics_executor);

    co_await WaitForServiceServer();

    for (size_t ii = 0; ii < bench_plans_.size(); ++ii) {
      if (!run_flag_.load()) break;

      co_await StartSinglePlan(ii, bench_plans_[ii]);

      co_await co::ScheduleAfter(statistics_executor, std::chrono::seconds(1));
    }

    MW_INFO("Bench completed.");

  } catch (const std::exception& e) {
    MW_ERROR("Exit MainLoop with exception, {}", e.what());
  }

  co_return;
}

co::Task<void> BenchmarkRpcClientModule::WaitForServiceServer() {
  MW_DEBUG("wait for service server...");

  auto statistics_executor = co::IbmwExecutor(client_statistics_executor_);
  co_await co::Schedule(statistics_executor);

  example_ros2::srv::RosTestRpc_Request req;
  example_ros2::srv::RosTestRpc_Response rsp;

  req.data = {'a', 'b', 'c'};

  while (run_flag_) {
    co_await co::ScheduleAfter(statistics_executor, std::chrono::seconds(1));

    // call rpc
    auto ctx_ptr = access_->NewContextSharedPtr();
    ctx_ptr->SetTimeout(std::chrono::seconds(3));

    auto status = co_await access_->RosTestRpc(ctx_ptr, req, rsp);

    if (!status.OK()) {
      MW_WARN("Server is not available!!!");
    } else {
      break;
    }
  }

  MW_DEBUG("Server is available!!!");

  co_return;
}

co::Task<void> BenchmarkRpcClientModule::StartSinglePlan(uint32_t plan_id, BenchPlan plan) {
  co::AsyncScope bench_scope;

  std::vector<std::vector<uint32_t>> perf_datas;
  perf_datas.resize(plan.parallel);
  for (uint32_t ii = 0; ii < plan.parallel; ii++) {
    perf_datas[ii].reserve(plan.msg_count);
  }

  auto start_time = std::chrono::steady_clock::now();

  // start coro
  for (uint32_t ii = 0; ii < plan.parallel; ii++) {
    if (plan.mode == BenchPlan::PerfMod::kFixedFreq) {
      bench_scope.spawn(co::On(co::InlineExecutor(), StartFixedFreqPlan(ii, plan, perf_datas[ii])));
    } else {
      bench_scope.spawn(co::On(co::InlineExecutor(), StartBenchPlan(ii, plan, perf_datas[ii])));
    }
  }

  auto statistics_executor = co::IbmwExecutor(client_statistics_executor_);
  co_await co::On(statistics_executor, bench_scope.complete());

  auto end_time = std::chrono::steady_clock::now();

  // statistics
  uint64_t total_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  std::vector<uint32_t> gather_vec;
  for (auto& vec : perf_datas) {
    gather_vec.insert(gather_vec.begin(), vec.begin(), vec.end());
  }

  uint64_t correct_count = gather_vec.size();
  uint64_t total_count = plan.msg_count * plan.parallel;

  double error_rate = static_cast<double>(total_count - correct_count) / total_count * 100;
  double qps = (total_count * 1000.0) / total_time_ms;

  std::sort(gather_vec.begin(), gather_vec.end());

  uint32_t min_latency = gather_vec.front();
  uint32_t max_latency = gather_vec.back();

  uint32_t p90_latency = gather_vec[static_cast<size_t>(correct_count * 0.9)];
  uint32_t p99_latency = gather_vec[static_cast<size_t>(correct_count * 0.99)];
  uint32_t p999_latency = gather_vec[static_cast<size_t>(correct_count * 0.999)];

  uint64_t sum_latency =
      std::accumulate<std::vector<uint32_t>::iterator, uint64_t>(gather_vec.begin(), gather_vec.end(), 0);
  uint32_t avg_latency = sum_latency / correct_count;

  if (plan.mode == BenchPlan::PerfMod::kFixedFreq) {
    MW_INFO(
        R"str(Benchmark plan {} completed, report:
mode: fixed-freq
freq: {}
msg size: {}
parallel: {}
msg count per co: {}
total count: {}
total time: {} ms
correct count: {}
error rate: {} %
min latency: {} us
max latency: {} us
avg latency: {} us
p90 latency: {} us
p99 latency: {} us
p999 latency: {} us
)str",
        plan_id,
        plan.freq,
        plan.msg_size,
        plan.parallel,
        plan.msg_count,
        total_count,
        total_time_ms,
        correct_count,
        error_rate,
        min_latency / 1000.0,
        max_latency / 1000.0,
        avg_latency / 1000.0,
        p90_latency / 1000.0,
        p99_latency / 1000.0,
        p999_latency / 1000.0);
  } else {
    MW_INFO(
        R"str(Benchmark plan {} completed, report:
mode: bench
msg size: {}
parallel: {}
msg count per co: {}
total count: {}
total time: {} ms
correct count: {}
error rate: {} %
qps: {}
min latency: {} us
max latency: {} us
avg latency: {} us
p90 latency: {} us
p99 latency: {} us
p999 latency: {} us
)str",
        plan_id,
        plan.msg_size,
        plan.parallel,
        plan.msg_count,
        total_count,
        total_time_ms,
        correct_count,
        error_rate,
        qps,
        min_latency / 1000.0,
        max_latency / 1000.0,
        avg_latency / 1000.0,
        p90_latency / 1000.0,
        p99_latency / 1000.0,
        p999_latency / 1000.0);
  }

  co_return;
}

co::Task<void> BenchmarkRpcClientModule::StartFixedFreqPlan(
    uint32_t parallel_id, BenchPlan plan, std::vector<uint32_t>& perf_data) {
  auto executor = executor_vec_[parallel_id];
  auto ibmw_executor = co::IbmwExecutor(executor);

  co_await co::Schedule(ibmw_executor);

  example_ros2::srv::RosTestRpc_Request req;
  example_ros2::srv::RosTestRpc_Response rsp;
  req.set__data(GenerateRandomString(plan.msg_size));

  uint32_t send_count = 0;

  uint32_t sleep_ns = static_cast<uint32_t>(1000000000 / plan.freq);
  auto cur_tp = executor.Now();

  for (; send_count < plan.msg_count; ++send_count) {
    if (!run_flag_.load()) [[unlikely]]
      break;

    auto ctx_ptr = access_->NewContextSharedPtr();
    ctx_ptr->SetTimeout(std::chrono::seconds(3));

    auto task_start_time = std::chrono::steady_clock::now();
    auto status = co_await access_->RosTestRpc(ctx_ptr, req, rsp);
    auto task_end_time = std::chrono::steady_clock::now();

    if (status.OK()) {
      if (task_end_time > task_start_time) {
        perf_data.emplace_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(task_end_time - task_start_time).count());
      } else {
        perf_data.emplace_back(0);
      }
    }

    cur_tp += std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(sleep_ns));
    co_await co::ScheduleAt(ibmw_executor, cur_tp);
  }

  co_return;
}

co::Task<void> BenchmarkRpcClientModule::StartBenchPlan(
    uint32_t parallel_id, BenchPlan plan, std::vector<uint32_t>& perf_data) {
  auto executor = executor_vec_[parallel_id];
  auto ibmw_executor = co::IbmwExecutor(executor);

  co_await co::Schedule(ibmw_executor);

  example_ros2::srv::RosTestRpc_Request req;
  example_ros2::srv::RosTestRpc_Response rsp;
  req.set__data(GenerateRandomString(plan.msg_size));

  uint32_t send_count = 0;

  for (; send_count < plan.msg_count; ++send_count) {
    if (!run_flag_.load()) [[unlikely]]
      break;

    auto ctx_ptr = access_->NewContextSharedPtr();
    ctx_ptr->SetTimeout(std::chrono::seconds(3));

    auto task_start_time = std::chrono::steady_clock::now();
    auto status = co_await access_->RosTestRpc(ctx_ptr, req, rsp);
    auto task_end_time = std::chrono::steady_clock::now();

    if (status.OK()) {
      if (task_end_time > task_start_time) {
        perf_data.emplace_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(task_end_time - task_start_time).count());
      } else {
        perf_data.emplace_back(0);
      }
    }

    co_await co::Schedule(ibmw_executor);
  }

  co_return;
}

}  // namespace ibmw::examples::cpp::ros2_rpc::benchmark_rpc_client_module
