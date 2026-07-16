// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <numeric>

#include "internal/typesupport/ros2/ros2_transport.h"
#include "benchmark_subscriber_module/benchmark_subscriber_module.h"
#include "base/format.h"
#include "base/string_util.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::examples::cpp::ros2_chn::benchmark_subscriber_module {

bool BenchmarkSubscriberModule::Setup(ibmw::Runtime core) {
  core_ = core;

  try {
    // Read cfg
    auto file_path = core_.GetConfigurator().GetConfigFilePath();
    if (!file_path.empty()) {
      YAML::Node cfg_node = YAML::LoadFile(std::string(file_path));
      max_topic_number_ = cfg_node["max_topic_number"].as<uint32_t>();
    }

    // Subscribe
    signal_subscriber_ = core_.GetTransport().GetSubscriber("benchmark_signal");
    MW_REQUIRE(signal_subscriber_, "Get subscriber for topic 'benchmark_signal' failed.");

    bool ret = ibmw::transport::OnMessage<example_ros2::msg::BenchmarkSignal>(
        signal_subscriber_, std::bind(&BenchmarkSubscriberModule::BenchmarkSignalHandle, this, std::placeholders::_1));
    MW_REQUIRE(ret, "Subscribe failed.");

    for (uint32_t ii = 0; ii < max_topic_number_; ++ii) {
      auto topic_name = "test_topic_" + std::to_string(ii);

      topic_record_vec_.emplace_back(TopicRecord{.topic_name = topic_name});

      auto subscriber = core_.GetTransport().GetSubscriber(topic_name);
      MW_REQUIRE(subscriber, "Get subscriber for topic '{}' failed.", topic_name);

      bool ret = ibmw::transport::OnMessage<example_ros2::msg::BenchmarkMessage>(
          subscriber,
          [this, ii](const std::shared_ptr<const example_ros2::msg::BenchmarkMessage>& data) {
            BenchmarkMessageHandle(ii, data);
          });
      MW_REQUIRE(ret, "Subscribe failed.");

      subscribers_.emplace_back(subscriber);
    }
  } catch (const std::exception& e) {
    MW_ERROR("Init failed, {}", e.what());
    return false;
  }

  MW_INFO("Setup succeeded.");

  return true;
}

bool BenchmarkSubscriberModule::Run() { return true; }

void BenchmarkSubscriberModule::Stop() {}

void BenchmarkSubscriberModule::BenchmarkSignalHandle(
    const std::shared_ptr<const example_ros2::msg::BenchmarkSignal>& data) {
  MW_INFO("Receive new signal data: {}", example_ros2::msg::to_yaml(*data));

  try {
    if (data->status == example_ros2::msg::BenchmarkStatus::BEGIN) {
      MW_REQUIRE(
          std::atomic_exchange(&run_state_, State::kRunning) == State::kReadyToRun,
          "Invalid state!");

      cur_bench_mode_ = data->mode;
      cur_bench_plan_id_ = data->bench_plan_id;
      cur_bench_topic_number_ = data->topic_number;
      cur_bench_parallel_number_ = data->parallel_number;
      cur_bench_expect_send_num_ = data->send_num;
      cur_bench_message_size_ = data->message_size;
      cur_bench_send_frequency_ = data->send_frequency;

      MW_REQUIRE(
          cur_bench_topic_number_ <= max_topic_number_,
          "Topic num({}) is greater than max topic number({})",
          cur_bench_topic_number_, max_topic_number_);

      for (size_t ii = 0; ii < cur_bench_topic_number_; ++ii) {
        topic_record_vec_[ii].msg_record_vec.clear();
        topic_record_vec_[ii].msg_record_vec.resize(cur_bench_expect_send_num_);
      }

    } else if (data->status == example_ros2::msg::BenchmarkStatus::END) {
      MW_REQUIRE(
          std::atomic_exchange(&run_state_, State::kEvaluating) == State::kRunning,
          "Invalid state!");

      Evaluate();

      MW_REQUIRE(
          std::atomic_exchange(&run_state_, State::kReadyToRun) == State::kEvaluating,
          "Invalid state!");
    }
  } catch (const std::exception& e) {
    MW_ERROR("Exception, {}", e.what());
  }
}

void BenchmarkSubscriberModule::BenchmarkMessageHandle(
    uint32_t topic_index,
    const std::shared_ptr<const example_ros2::msg::BenchmarkMessage>& data) {
  auto recv_timestamp = ibmw::common::util::GetCurTimestampNs();

  auto& topic_record = topic_record_vec_[topic_index];

  if (run_state_.load() != State::kRunning) [[unlikely]] {
    MW_WARN("Topic '{}' is end bench.", topic_record.topic_name);
    return;
  }

  auto data_size = static_cast<uint32_t>(data->data.size());
  if (data_size != cur_bench_message_size_) [[unlikely]] {
    MW_WARN("Topic '{}' get error data size {}, expect {}.",
               topic_record.topic_name, data_size, cur_bench_message_size_);
    return;
  }

  auto seq = data->seq;

  auto& msg_record_vec = topic_record.msg_record_vec;

  if (seq >= msg_record_vec.size()) [[unlikely]] {
    MW_WARN("Invalid seq {}, Topic '{}'.", seq, topic_record.topic_name);
    return;
  }

  auto& msg_record = msg_record_vec[seq];

  msg_record.recv = true;
  msg_record.send_timestamp = data->timestamp;
  msg_record.recv_timestamp = recv_timestamp;
}

void BenchmarkSubscriberModule::Evaluate() const {
  MW_INFO("Benchmark plan {} completed, evaluate...", cur_bench_plan_id_);

  size_t send_count = cur_bench_expect_send_num_ * cur_bench_topic_number_;

  std::vector<uint32_t> statistics_array;
  statistics_array.reserve(send_count);

  uint64_t sum_latency = 0;

  for (size_t ii = 0; ii < cur_bench_topic_number_; ++ii) {
    for (const auto& msg_record : topic_record_vec_[ii].msg_record_vec) {
      if (!msg_record.recv) [[unlikely]]
        continue;

      uint32_t latency = 0;
      if (msg_record.recv_timestamp < msg_record.send_timestamp) [[unlikely]] {
        MW_WARN("Invalid timestamp, recv timestamp: {}, send timestamp: {}",
                   msg_record.recv_timestamp, msg_record.send_timestamp);

      } else {
        latency = static_cast<uint32_t>(msg_record.recv_timestamp - msg_record.send_timestamp);
      }

      sum_latency += latency;
      statistics_array.emplace_back(latency);
    }
  }

  size_t recv_count = statistics_array.size();

  double loss_rate = static_cast<double>(send_count - recv_count) / send_count * 100.0;

  std::sort(statistics_array.begin(), statistics_array.end());

  uint32_t min_latency = statistics_array.front();
  uint32_t max_latency = statistics_array.back();

  uint32_t p90_latency = statistics_array[static_cast<size_t>(recv_count * 0.9)];
  uint32_t p99_latency = statistics_array[static_cast<size_t>(recv_count * 0.99)];
  uint32_t p999_latency = statistics_array[static_cast<size_t>(recv_count * 0.999)];

  uint32_t avg_latency = sum_latency / recv_count;

  MW_INFO(R"str(Benchmark plan {} completed, report:
mode: {}
frequency: {} hz
topic number: {}
parallel number: {}
msg size: {} bytes
msg count per topic: {}
send count : {}
recv count: {}
loss rate: {} %
min latency: {} us
max latency: {} us
avg latency: {} us
p90 latency: {} us
p99 latency: {} us
p999 latency: {} us
)str",
             cur_bench_plan_id_,
             cur_bench_mode_,
             cur_bench_send_frequency_,
             cur_bench_topic_number_,
             cur_bench_parallel_number_,
             cur_bench_message_size_,
             cur_bench_expect_send_num_,
             send_count,
             recv_count,
             loss_rate,
             min_latency / 1000.0,
             max_latency / 1000.0,
             avg_latency / 1000.0,
             p90_latency / 1000.0,
             p99_latency / 1000.0,
             p999_latency / 1000.0);
}

}  // namespace ibmw::examples::cpp::ros2_chn::benchmark_subscriber_module
