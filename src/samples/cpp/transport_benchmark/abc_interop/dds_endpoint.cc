#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <std_msgs/msg/u_int8_multi_array.hpp>

#include "base/benchmark_util.h"
#include "cpp/core.h"
#include "cpp/module_base.h"
#include "engine/engine.h"
#include "uint8_multi_array_serializer.h"
#include "type_support/ros2/ros2_transport.h"

namespace {
constexpr size_t kPayloadBytes = 4096;
constexpr uint8_t kFillerMagic = 0xA5;
constexpr char kTopic[] = "/mw_perf/payload_4kb";
constexpr char kType[] = "std_msgs/msg/UInt8MultiArray";

struct Options {
  std::string role;
  std::string config;
  std::string topic{kTopic};
  std::string csv;
  std::string status;
  uint32_t hz{100};
  uint32_t warmup{50};
  uint32_t count{3000};
};
Options g_options;
ibmw::runtime::core::Engine* g_engine = nullptr;

bool Uint(const char* text, uint32_t* value) {
  try {
    size_t used = 0;
    const auto parsed = std::stoul(text, &used);
    if (used != std::strlen(text) || parsed == 0 || parsed > UINT32_MAX) return false;
    *value = static_cast<uint32_t>(parsed);
    return true;
  } catch (...) { return false; }
}

bool Parse(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h" || i + 1 >= argc) return false;
    if (arg == "--role") g_options.role = argv[++i];
    else if (arg == "--config") g_options.config = argv[++i];
    else if (arg == "--topic") g_options.topic = argv[++i];
    else if (arg == "--csv") g_options.csv = argv[++i];
    else if (arg == "--status") g_options.status = argv[++i];
    else if (arg == "--hz") {
      if (!Uint(argv[++i], &g_options.hz)) return false;
    }
    else if (arg == "--warmup-messages") {
      if (!Uint(argv[++i], &g_options.warmup)) return false;
    }
    else if (arg == "--message-count") {
      if (!Uint(argv[++i], &g_options.count)) return false;
    }
    else return false;
  }
  return (g_options.role == "pub" || g_options.role == "sub") &&
         !g_options.config.empty() && g_options.topic == kTopic;
}

void Usage(const char* program) {
  std::cerr << "Usage: " << program
            << " --role pub|sub --config FILE [--topic " << kTopic
            << "] [--hz N] [--warmup-messages N] [--message-count N] [--csv PATH]\n";
}

class PublisherModule final : public ibmw::Module {
 public:
  ibmw::ModuleDescriptor Info() const override {
    return {.module_name = "AbcDdsPublisherModule"};
  }
  bool Setup(ibmw::Runtime core) override {
    sender_ = core.GetTransport().GetPublisher(g_options.topic);
    return ibmw::ros2::transport::RegisterSendType<std_msgs::msg::UInt8MultiArray>(
        sender_, kType);
  }
  bool Run() override {
    running_ = true;
    thread_ = std::thread([this] {
      const auto period = std::chrono::nanoseconds(
          1'000'000'000ULL / std::max<uint32_t>(g_options.hz, 1));
      auto next = std::chrono::steady_clock::now();
      for (uint64_t seq = 0;
           running_ && seq < g_options.warmup + g_options.count; ++seq) {
        std_msgs::msg::UInt8MultiArray msg;
        msg.data.assign(kPayloadBytes, kFillerMagic);
        const uint64_t send_ns = ibmw::common::util::MonotonicNs();
        std::memcpy(msg.data.data(), &send_ns, sizeof(send_ns));
        std::memcpy(msg.data.data() + sizeof(send_ns), &seq, sizeof(seq));
        ibmw::ros2::transport::Send(sender_, kType, msg);
        next += period;
        std::this_thread::sleep_until(next);
      }
      if (g_engine) g_engine->Shutdown();
      if (!g_options.status.empty()) {
        std::ofstream out(g_options.status, std::ios::trunc);
        out << "{\n  \"attempted_total\": " << (g_options.warmup + g_options.count)
            << ",\n  \"attempted_measured\": " << g_options.count << "\n}\n";
      }
    });
    return true;
  }
  void Stop() override {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }
 private:
  ibmw::transport::SenderRef sender_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

class SubscriberModule final : public ibmw::Module {
 public:
  SubscriberModule() : stats_("abc_dds_4KB"), seen_(g_options.count, false) { stats_.Reserve(g_options.count); }
  ibmw::ModuleDescriptor Info() const override {
    return {.module_name = "AbcDdsSubscriberModule"};
  }
  bool Setup(ibmw::Runtime core) override {
    auto receiver = core.GetTransport().GetSubscriber(g_options.topic);
    return ibmw::ros2::transport::OnMessage<std_msgs::msg::UInt8MultiArray>(
        receiver, kType,
        [this](ibmw::transport::ContextRef,
               const std::shared_ptr<const std_msgs::msg::UInt8MultiArray>& msg) {
          if (!msg || msg->data.size() != kPayloadBytes) { std::lock_guard<std::mutex> lock(mu_); ++invalid_payload_; return; }
          bool zero_filler = true, magic_filler = true;
          for (size_t i = 16; i < msg->data.size(); ++i) {
            zero_filler = zero_filler && msg->data[i] == 0;
            magic_filler = magic_filler && msg->data[i] == kFillerMagic;
          }
          if (!zero_filler && !magic_filler) { std::lock_guard<std::mutex> lock(mu_); ++invalid_payload_; return; }
          uint64_t send_ns = 0, seq = 0;
          std::memcpy(&send_ns, msg->data.data(), sizeof(send_ns));
          std::memcpy(&seq, msg->data.data() + sizeof(send_ns), sizeof(seq));
          if (seq < g_options.warmup || seq >= static_cast<uint64_t>(g_options.warmup) + g_options.count) return;
          const auto now = ibmw::common::util::MonotonicNs();
          std::lock_guard<std::mutex> lock(mu_);
          ++received_;
          if (send_ns == 0 || send_ns > now) { ++invalid_timestamp_; return; }
          const size_t index = static_cast<size_t>(seq - g_options.warmup);
          if (have_last_ && seq <= last_seq_) ++out_of_order_;
          last_seq_ = seq; have_last_ = true;
          if (seen_[index]) { ++duplicate_; return; }
          seen_[index] = true; ++unique_;
          stats_.Add(now - send_ns);
          if (unique_ == g_options.count && g_engine) g_engine->Shutdown();
        });
  }
  bool Run() override { return true; }
  void Stop() override {
    std::lock_guard<std::mutex> lock(mu_);
    std::cout << stats_.ToHumanString() << '\n';
    if (!g_options.status.empty()) {
      std::ofstream out(g_options.status, std::ios::trunc);
      out << "{\n  \"attempted_total\": " << (g_options.warmup + g_options.count)
          << ",\n  \"attempted_measured\": " << g_options.count
          << ",\n  \"received_measured\": " << received_
          << ",\n  \"unique_measured\": " << unique_
          << ",\n  \"missing\": " << (g_options.count > unique_ ? g_options.count - unique_ : 0)
          << ",\n  \"duplicate\": " << duplicate_ << ",\n  \"out_of_order\": " << out_of_order_ << ",\n"
          << "  \"invalid_payload\": " << invalid_payload_ << ",\n  \"invalid_timestamp\": " << invalid_timestamp_ << ",\n"
          << "  \"valid\": " << (unique_ == g_options.count && received_ == g_options.count && duplicate_ == 0 && out_of_order_ == 0 && invalid_payload_ == 0 && invalid_timestamp_ == 0 ? "true" : "false") << "\n}\n";
    }
    if (unique_ == g_options.count && received_ == g_options.count && duplicate_ == 0 && out_of_order_ == 0 && invalid_payload_ == 0 && invalid_timestamp_ == 0 && !g_options.csv.empty()) {
      std::ifstream check(g_options.csv, std::ios::ate);
      const bool empty = !check || check.tellg() == 0;
      std::ofstream out(g_options.csv, std::ios::app);
      if (empty) out << ibmw::common::util::LatencyStats::kCsvHeader;
      stats_.AppendCsvRow(out);
    }
  }
 private:
  std::mutex mu_;
  uint32_t received_{0}, unique_{0}, duplicate_{0}, out_of_order_{0}, invalid_payload_{0}, invalid_timestamp_{0};
  uint64_t last_seq_{0}; bool have_last_{false};
  std::vector<bool> seen_;
  ibmw::common::util::LatencyStats stats_;
};

void SignalHandler(int signal) {
  if (g_engine && (signal == SIGINT || signal == SIGTERM)) g_engine->Shutdown();
}
}  // namespace

int main(int argc, char** argv) {
  if (!Parse(argc, argv)) { Usage(argv[0]); return 2; }
  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);
  try {
    ibmw::runtime::core::Engine engine;
    g_engine = &engine;
    PublisherModule publisher;
    SubscriberModule subscriber;
    if (g_options.role == "pub")
      engine.GetModuleBus().RegisterModule(publisher.NativeHandle());
    else
      engine.GetModuleBus().RegisterModule(subscriber.NativeHandle());
    ibmw::runtime::core::Engine::Options options;
    options.cfg_file_path = g_options.config;
    engine.Initialize(options);
    engine.Start();
    engine.Shutdown();
    g_engine = nullptr;
  } catch (const std::exception& error) {
    std::cerr << "B endpoint exception: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
