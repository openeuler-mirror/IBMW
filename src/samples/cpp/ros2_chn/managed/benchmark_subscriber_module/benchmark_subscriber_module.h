// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"

#include "example_ros2/msg/benchmark_message.hpp"
#include "example_ros2/msg/benchmark_signal.hpp"
#include "example_ros2/msg/benchmark_status.hpp"

namespace ibmw::examples::cpp::ros2_chn::benchmark_subscriber_module {

class BenchmarkSubscriberModule : public ibmw::Module {
 public:
  BenchmarkSubscriberModule() = default;
  ~BenchmarkSubscriberModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "BenchmarkSubscriberModule"};
  }

  bool Setup(ibmw::Runtime core) override;

  bool Run() override;

  void Stop() override;

 private:
  auto GetLogger() const { return core_.GetLogger(); }

  void BenchmarkSignalHandle(
      const std::shared_ptr<const example_ros2::msg::BenchmarkSignal>& data);
  void BenchmarkMessageHandle(
      uint32_t topic_index,
      const std::shared_ptr<const example_ros2::msg::BenchmarkMessage>& data);

  void Evaluate() const;

 private:
  ibmw::Runtime core_;

  ibmw::transport::ReceiverRef signal_subscriber_;  // topic name: benchmark_signal

  uint32_t max_topic_number_;
  std::vector<ibmw::transport::ReceiverRef> subscribers_;  // name: test_topic_x

  enum class State : uint32_t {
    kReadyToRun,
    kRunning,
    kEvaluating,
  };
  std::atomic<State> run_state_ = State::kReadyToRun;

  std::string cur_bench_mode_;
  uint32_t cur_bench_plan_id_ = 0;
  uint32_t cur_bench_topic_number_ = 0;
  uint32_t cur_bench_parallel_number_ = 0;
  uint32_t cur_bench_expect_send_num_ = 0;
  uint32_t cur_bench_message_size_ = 0;
  uint32_t cur_bench_send_frequency_ = 0;

  struct TopicRecord {
    std::string topic_name;

    struct MsgRecord {
      bool recv = false;
      uint64_t send_timestamp = 0;
      uint64_t recv_timestamp = 0;
    };
    std::vector<MsgRecord> msg_record_vec;
  };
  std::vector<TopicRecord> topic_record_vec_;
};

}  // namespace ibmw::examples::cpp::ros2_chn::benchmark_subscriber_module
