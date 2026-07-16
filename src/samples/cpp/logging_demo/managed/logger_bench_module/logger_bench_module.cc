// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#include "logger_bench_module/logger_bench_module.h"
#include "base/string_util.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::logger::logger_bench_module {
bool LoggerBenchModule::Setup(ibmw::Runtime core) {
  core_ = core;
  auto file_path = core_.GetConfigurator().GetConfigFilePath();
  if (!file_path.empty()) {
    YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
    log_data_size_vec_ = cfg_node["log_data_size"].as<std::vector<uint32_t>>();
    log_bench_num_ = cfg_node["log_bench_num"].as<uint32_t>();
  }
  auto work_executor = core_.GetExecutor().GetExecutor("work_executor");
  MW_REQUIRE(work_executor, "Get executor 'work_thread_pool' failed.");
  MW_INFO("Setup succeeded.");
  return true;
}
bool LoggerBenchModule::Run() {
  run_flag_ = true;
  MW_INFO("Run succeeded.");
  auto work_executor = core_.GetExecutor().GetExecutor("work_executor");
  work_executor.Dispatch([this, work_executor]() {
    using namespace std::chrono;
    std::vector<std::vector<std::string>> result_table = {{"log num", "log data size", "total duration(us)", "avg latency(ns)"}};
    size_t bench_num = log_data_size_vec_.size();
    for (size_t bench_count = 0; bench_count < bench_num; ++bench_count) {
      uint32_t log_data_size = log_data_size_vec_[bench_count];
      std::string log_data;
      log_data.reserve(log_data_size);
      for (size_t ii = 0; ii < log_data_size; ++ii) { log_data += std::to_string(ii % 10); }
      auto start_time_point = steady_clock::now();
      for (size_t ii = 0; ii < log_bench_num_; ++ii) { MW_INFO("{}", log_data); }
      auto end_time_point = steady_clock::now();
      auto total_duration = end_time_point - start_time_point;
      result_table.emplace_back(std::vector<std::string>{
          std::to_string(log_bench_num_), std::to_string(log_data_size),
          std::to_string(duration_cast<nanoseconds>(total_duration).count()),
          std::to_string(duration_cast<nanoseconds>(total_duration).count() / log_bench_num_)});
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    MW_INFO("report:\n{}", ibmw::common::util::FormatTree(result_table));
    log_loop_stop_sig_.set_value();
  });
  return true;
}
void LoggerBenchModule::Stop() {
  if (run_flag_) {
    run_flag_ = false;
    log_loop_stop_sig_.get_future().wait();
  }
  MW_INFO("Stop succeeded.");
}
}
