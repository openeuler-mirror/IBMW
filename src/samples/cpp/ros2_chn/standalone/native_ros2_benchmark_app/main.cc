// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "base/benchmark_util.h"
#include "example_ros2/msg/benchmark_message.hpp"
#include "rclcpp/rclcpp.hpp"

namespace {

using BenchmarkMessage = example_ros2::msg::BenchmarkMessage;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string role = "both";
  std::string executor = "multi";
  uint32_t threads = 4;
  uint32_t msg_size = 4096;
  uint32_t msg_count = 1024;
  uint32_t frequency = 1000;
  uint32_t warmup_count = 10;
  uint32_t wait_after_send_ms = 2000;
  uint32_t wait_for_match_ms = 10000;
  uint32_t listen_ms = 15000;
  std::string topic = "native_ros2_benchmark_topic";
  std::string label = "native_ros2";
  std::string csv_path;
  std::string reliability = "reliable";
};

struct Samples {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<uint64_t> latencies_ns;
  std::vector<bool> received;
  std::atomic<uint32_t> recv_count{0};

  explicit Samples(uint32_t count) : latencies_ns(count, 0), received(count, false) {}
};

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch()).count());
}

void PrintUsage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " [--role both|pub|sub] [--executor single|multi] [--threads N]"
            << " [--msg-size N] [--msg-count N] [--frequency N] [--topic NAME]"
            << " [--wait-after-send-ms N] [--wait-for-match-ms N] [--listen-ms N]"
            << " [--warmup-count N] [--reliability reliable|best_effort]"
            << " [--label LABEL] [--csv-path PATH]\n";
}

bool ParseUint(const std::string& value, uint32_t* out) {
  char* end = nullptr;
  unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0') return false;
  *out = static_cast<uint32_t>(parsed);
  return true;
}

bool ParseArgs(int argc, char** argv, Options* options) {
  for (int ii = 1; ii < argc; ++ii) {
    std::string arg = argv[ii];
    auto require_value = [&](const char* name) -> const char* {
      if (ii + 1 >= argc) {
        std::cerr << "Missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++ii];
    };

    if (arg == "--role") {
      const char* value = require_value("--role");
      if (value == nullptr) return false;
      options->role = value;
    } else if (arg == "--executor") {
      const char* value = require_value("--executor");
      if (value == nullptr) return false;
      options->executor = value;
    } else if (arg == "--threads") {
      const char* value = require_value("--threads");
      if (value == nullptr || !ParseUint(value, &options->threads)) return false;
    } else if (arg == "--msg-size") {
      const char* value = require_value("--msg-size");
      if (value == nullptr || !ParseUint(value, &options->msg_size)) return false;
    } else if (arg == "--msg-count") {
      const char* value = require_value("--msg-count");
      if (value == nullptr || !ParseUint(value, &options->msg_count)) return false;
    } else if (arg == "--frequency") {
      const char* value = require_value("--frequency");
      if (value == nullptr || !ParseUint(value, &options->frequency)) return false;
    } else if (arg == "--warmup-count") {
      const char* value = require_value("--warmup-count");
      if (value == nullptr || !ParseUint(value, &options->warmup_count)) return false;
    } else if (arg == "--reliability") {
      const char* value = require_value("--reliability");
      if (value == nullptr) return false;
      options->reliability = value;
      if (options->reliability != "reliable" &&
          options->reliability != "best_effort") {
        std::cerr << "Invalid --reliability: " << options->reliability << "\n";
        return false;
      }
    } else if (arg == "--topic") {
      const char* value = require_value("--topic");
      if (value == nullptr) return false;
      options->topic = value;
    } else if (arg == "--label") {
      const char* value = require_value("--label");
      if (value == nullptr) return false;
      options->label = value;
    } else if (arg == "--csv-path") {
      const char* value = require_value("--csv-path");
      if (value == nullptr) return false;
      options->csv_path = value;
    } else if (arg == "--wait-after-send-ms") {
      const char* value = require_value("--wait-after-send-ms");
      if (value == nullptr || !ParseUint(value, &options->wait_after_send_ms)) return false;
    } else if (arg == "--wait-for-match-ms") {
      const char* value = require_value("--wait-for-match-ms");
      if (value == nullptr || !ParseUint(value, &options->wait_for_match_ms)) return false;
    } else if (arg == "--listen-ms") {
      const char* value = require_value("--listen-ms");
      if (value == nullptr || !ParseUint(value, &options->listen_ms)) return false;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return false;
    } else {
      std::cerr << "Unknown argument: " << arg << "\n";
      return false;
    }
  }

  if ((options->role != "both" && options->role != "pub" && options->role != "sub") ||
      (options->executor != "single" && options->executor != "multi") ||
      options->threads == 0 || options->msg_count == 0 || options->frequency == 0) {
    return false;
  }
  return true;
}

std::shared_ptr<rclcpp::Executor> CreateExecutor(const Options& options) {
  if (options.executor == "single") {
    return std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
  }
  return std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
      rclcpp::ExecutorOptions(), options.threads);
}

rclcpp::QoS CreateQos(const Options& options) {
  rclcpp::QoS qos(options.msg_count + 16);
  if (options.reliability == "best_effort") {
    qos.best_effort();
  } else {
    qos.reliable();
  }
  qos.keep_last(options.msg_count + 16);
  return qos;
}

uint64_t Percentile(const std::vector<uint64_t>& sorted, double pct) {
  if (sorted.empty()) return 0;
  // Keep the same coarse bucket-index percentile convention used by the
  // IBMW benchmark reporters; this is not an interpolated quantile.
  size_t index = static_cast<size_t>(static_cast<double>(sorted.size()) * pct);
  if (index >= sorted.size()) index = sorted.size() - 1;
  return sorted[index];
}

int PrintReport(const Options& options, const Samples& samples) {
  std::vector<uint64_t> values;
  values.reserve(options.msg_count);
  ibmw::common::util::LatencyStats stats(options.label);
  stats.Reserve(options.msg_count);
  for (uint32_t ii = 0; ii < options.msg_count; ++ii) {
    if (samples.received[ii]) {
      values.push_back(samples.latencies_ns[ii]);
      stats.Add(samples.latencies_ns[ii]);
    }
  }

  std::sort(values.begin(), values.end());
  const uint32_t received_count = static_cast<uint32_t>(values.size());
  const uint32_t dropped = options.msg_count - received_count;
  const double loss_rate = static_cast<double>(dropped) * 100.0 /
                           static_cast<double>(options.msg_count);

  std::cout << "Native ROS2 benchmark completed, report:\n";
  std::cout << "role: " << options.role << "\n";
  std::cout << "executor: " << options.executor << "\n";
  std::cout << "threads: " << (options.executor == "multi" ? options.threads : 1) << "\n";
  std::cout << "frequency: " << options.frequency << " hz\n";
  std::cout << "warmup count: " << options.warmup_count << "\n";
  std::cout << "msg size: " << options.msg_size << " bytes\n";
  std::cout << "send count : " << options.msg_count << "\n";
  std::cout << "recv count: " << received_count << "\n";
  std::cout << "label: " << options.label << "\n";
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "loss rate: " << loss_rate << " %\n";

  if (!options.csv_path.empty()) {
    bool need_header = false;
    {
      std::ifstream peek(options.csv_path, std::ios::binary | std::ios::ate);
      if (!peek.is_open() || peek.tellg() == std::streampos(0)) need_header = true;
    }
    std::ofstream out(options.csv_path, std::ios::binary | std::ios::app);
    if (!out.is_open()) {
      std::cerr << "failed to open CSV path for append: " << options.csv_path << "\n";
      return 2;
    }
    if (need_header) out << ibmw::common::util::LatencyStats::kCsvHeader;
    stats.AppendCsvRow(out);
    std::cout << "csv path: " << options.csv_path << "\n";
  }

  if (values.empty()) {
    std::cout << "no latency samples\n";
    return 2;
  }

  const uint64_t sum = std::accumulate(values.begin(), values.end(), uint64_t{0});
  std::cout << "min latency: " << values.front() / 1000.0 << " us\n";
  std::cout << "max latency: " << values.back() / 1000.0 << " us\n";
  std::cout << "avg latency: " << (sum / static_cast<double>(values.size())) / 1000.0 << " us\n";
  std::cout << "p50 latency: " << Percentile(values, 0.50) / 1000.0 << " us\n";
  std::cout << "p90 latency: " << Percentile(values, 0.90) / 1000.0 << " us\n";
  std::cout << "p99 latency: " << Percentile(values, 0.99) / 1000.0 << " us\n";
  std::cout << "p999 latency: " << Percentile(values, 0.999) / 1000.0 << " us\n";

  return dropped == 0 ? 0 : 1;
}

rclcpp::Subscription<BenchmarkMessage>::SharedPtr CreateSubscription(
    const Options& options,
    const std::shared_ptr<rclcpp::Node>& node,
    Samples* samples) {
  return node->create_subscription<BenchmarkMessage>(
      options.topic,
      CreateQos(options),
      [samples, count = options.msg_count](const BenchmarkMessage::SharedPtr msg) {
        const uint64_t recv_timestamp = NowNs();
        if (msg->seq >= count) return;

        std::lock_guard<std::mutex> lock(samples->mutex);
        if (samples->received[msg->seq]) return;
        samples->received[msg->seq] = true;
        samples->latencies_ns[msg->seq] = recv_timestamp >= msg->timestamp
                                             ? recv_timestamp - msg->timestamp
                                             : 0;
        samples->recv_count.fetch_add(1, std::memory_order_relaxed);
        samples->cv.notify_one();
      });
}

bool WaitForSubscriber(
    const rclcpp::Publisher<BenchmarkMessage>::SharedPtr& publisher,
    std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (publisher->get_subscription_count() > 0) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

void PublishMessages(
    const Options& options,
    const rclcpp::Publisher<BenchmarkMessage>::SharedPtr& publisher) {
  BenchmarkMessage warmup;
  warmup.data.resize(options.msg_size, 0x5a);
  for (uint32_t ii = 0; ii < options.warmup_count; ++ii) {
    warmup.seq = options.msg_count + ii;
    warmup.timestamp = NowNs();
    publisher->publish(warmup);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  BenchmarkMessage msg;
  msg.data.resize(options.msg_size, 0x42);
  const auto period = std::chrono::nanoseconds(1000000000ULL / options.frequency);
  auto next_send = Clock::now();
  for (uint32_t seq = 0; seq < options.msg_count; ++seq) {
    msg.seq = seq;
    msg.timestamp = NowNs();
    publisher->publish(msg);
    next_send += period;
    std::this_thread::sleep_until(next_send);
  }
}

int RunPublisherOnly(const Options& options) {
  auto node = std::make_shared<rclcpp::Node>("native_ros2_benchmark_pub");
  auto publisher = node->create_publisher<BenchmarkMessage>(options.topic, CreateQos(options));
  auto executor = CreateExecutor(options);
  executor->add_node(node);
  std::thread spin_thread([&]() { executor->spin(); });

  if (!WaitForSubscriber(publisher, std::chrono::milliseconds(options.wait_for_match_ms))) {
    std::cerr << "Native ROS2 publisher failed: no matched subscriber before publish\n";
    executor->cancel();
    if (spin_thread.joinable()) spin_thread.join();
    return 1;
  }

  PublishMessages(options, publisher);
  std::this_thread::sleep_for(std::chrono::milliseconds(options.wait_after_send_ms));
  executor->cancel();
  if (spin_thread.joinable()) spin_thread.join();
  return 0;
}

int RunSubscriberOnly(const Options& options) {
  auto node = std::make_shared<rclcpp::Node>("native_ros2_benchmark_sub");
  Samples samples(options.msg_count);
  auto subscription = CreateSubscription(options, node, &samples);
  auto executor = CreateExecutor(options);
  executor->add_node(node);
  std::thread spin_thread([&]() { executor->spin(); });

  {
    std::unique_lock<std::mutex> lock(samples.mutex);
    samples.cv.wait_for(lock, std::chrono::milliseconds(options.listen_ms), [&]() {
      return samples.recv_count.load(std::memory_order_relaxed) >= options.msg_count;
    });
  }

  executor->cancel();
  if (spin_thread.joinable()) spin_thread.join();
  return PrintReport(options, samples);
}

int RunCombined(const Options& options) {
  auto pub_node = std::make_shared<rclcpp::Node>("native_ros2_benchmark_pub");
  auto sub_node = std::make_shared<rclcpp::Node>("native_ros2_benchmark_sub");
  Samples samples(options.msg_count);
  auto subscription = CreateSubscription(options, sub_node, &samples);
  auto publisher = pub_node->create_publisher<BenchmarkMessage>(options.topic, CreateQos(options));
  auto executor = CreateExecutor(options);
  executor->add_node(pub_node);
  executor->add_node(sub_node);
  std::thread spin_thread([&]() { executor->spin(); });

  std::this_thread::sleep_for(std::chrono::seconds(1));
  PublishMessages(options, publisher);
  {
    std::unique_lock<std::mutex> lock(samples.mutex);
    samples.cv.wait_for(lock, std::chrono::milliseconds(options.wait_after_send_ms), [&]() {
      return samples.recv_count.load(std::memory_order_relaxed) >= options.msg_count;
    });
  }

  executor->cancel();
  if (spin_thread.joinable()) spin_thread.join();
  return PrintReport(options, samples);
}

int RunBenchmark(const Options& options) {
  if (options.role == "pub") return RunPublisherOnly(options);
  if (options.role == "sub") return RunSubscriberOnly(options);
  return RunCombined(options);
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage(argv[0]);
    return 2;
  }

  rclcpp::init(argc, argv);
  int result = 1;
  try {
    result = RunBenchmark(options);
  } catch (const std::exception& e) {
    std::cerr << "Native ROS2 benchmark failed: " << e.what() << "\n";
    result = 1;
  }
  rclcpp::shutdown();
  return result;
}
