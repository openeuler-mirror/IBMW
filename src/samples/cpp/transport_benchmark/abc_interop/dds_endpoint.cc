#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <std_msgs/msg/u_int8_multi_array.hpp>

#include "base/benchmark_util.h"
#include "cpp/core.h"
#include "cpp/module_base.h"
#include "dds_ext/dds_transport_driver.h"
#include "dds_ext/ros2_type_helper.h"
#include "engine/engine.h"
#include "ros2_latency_artifact.h"
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
  std::string jsonl;
  uint32_t hz{100};
  uint32_t warmup{50};
  uint32_t count{3000};
};
Options g_options;
using ibmw::runtime::core::Engine;
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

bool PositiveUint64(const char* text, uint64_t* value) {
  if (text == nullptr || *text == '\0') return false;
  uint64_t parsed = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
    const uint64_t digit = static_cast<uint64_t>(*p - '0');
    if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10)
      return false;
    parsed = parsed * 10 + digit;
  }
  if (parsed == 0) return false;
  *value = parsed;
  return true;
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
    else if (arg == "--jsonl") g_options.jsonl = argv[++i];
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
            << "] [--hz N] [--warmup-messages N] [--message-count N] [--csv PATH]"
            << " [--status PATH] [--jsonl PATH]\n";
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
  bool PublisherOk() const { return publisher_ok_; }
  bool Run() override {
    running_ = true;
    thread_ = std::thread([this] {
      if (!WaitForSubscriberMatch()) {
        publisher_ok_ = false;
        if (g_engine) g_engine->Shutdown();
        return;
      }
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
  bool WaitForSubscriberMatch() {
    using clock = std::chrono::steady_clock;
    constexpr auto kMatchTimeout = std::chrono::seconds(10);
    constexpr auto kMatchPoll = std::chrono::milliseconds(50);

    if (driver_ == nullptr) {
      std::cerr << "B publisher DDS driver is null before match wait\n";
      return false;
    }

    const std::string dds_topic =
        ibmw::extensions::dds_extension::ROS2TypeHelper::ConvertToDdsTopicName(
            g_options.topic, driver_->GetOptions().ros2_topic_prefix);
    const auto deadline = clock::now() + kMatchTimeout;
    bool query_succeeded = false;
    int matched_count = -1;
    while (running_ && clock::now() <= deadline) {
      matched_count = driver_->GetPublicationMatchedCount(dds_topic);
      if (matched_count >= 0) query_succeeded = true;
      if (matched_count > 0) return true;
      std::this_thread::sleep_for(kMatchPoll);
    }

    if (!running_) {
      std::cerr << "B publisher subscriber match wait cancelled\n";
    } else if (!query_succeeded) {
      std::cerr << "B publisher DDS match query failed for 10 seconds: "
                << dds_topic << '\n';
    } else {
      std::cerr << "B publisher timed out after 10 seconds waiting for subscriber: "
                << dds_topic << " current_count=" << matched_count << '\n';
    }
    return false;
  }

  ibmw::transport::SenderRef sender_;
  ibmw::extensions::dds_extension::DdsTransportDriver* driver_{nullptr};
  std::atomic<bool> running_{false};
  std::atomic<bool> publisher_ok_{true};
  std::thread thread_;
  IBMW_USE_DRIVER(ibmw::extensions::dds_extension::DdsTransportDriver, driver_)
};

class SubscriberModule final : public ibmw::Module {
 public:
  SubscriberModule()
      : stats_("abc_dds_4KB"),
        seen_(g_options.count, false),
        latency_records_(g_options.count) {
    stats_.Reserve(g_options.count);
    if (!g_options.jsonl.empty()) {
      const char* run = std::getenv("MW_PERF_RUN_ID");
      const char* schema = std::getenv("MW_PERF_ARTIFACT_SCHEMA_VERSION");
      const char* producer_pid_env = std::getenv("MW_PERF_PRODUCER_PID");
      const char* producer_start_ticks_env =
          std::getenv("MW_PERF_PRODUCER_START_TIME_TICKS");
      run_id_ = run ? run : "";
      artifact_ready_ = !run_id_.empty() && schema &&
                        std::string(schema) == ibmw::abc::kLatencySchema &&
                        PositiveUint64(producer_pid_env, &producer_pid_) &&
                        PositiveUint64(producer_start_ticks_env,
                                       &producer_start_ticks_);
      jsonl_.open(g_options.jsonl, std::ios::out | std::ios::trunc);
      artifact_ready_ = artifact_ready_ && static_cast<bool>(jsonl_);
      if (!artifact_ready_)
        std::cerr << "Failed to initialize strict latency artifact\n";
    }
  }
  bool ArtifactReady() const { return artifact_ready_; }
  bool ArtifactOk() const { return artifact_ok_; }
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
          latency_records_[index] = {seq, send_ns, now, msg->data.size()};
          if (unique_ == g_options.count && g_engine) g_engine->Shutdown();
        });
  }
  bool Run() override { return true; }
  void Stop() override {
    std::lock_guard<std::mutex> lock(mu_);
    std::cout << stats_.ToHumanString() << '\n';
    const bool valid = unique_ == g_options.count && received_ == g_options.count &&
                       duplicate_ == 0 && out_of_order_ == 0 &&
                       invalid_payload_ == 0 && invalid_timestamp_ == 0;
    if (!g_options.jsonl.empty() && artifact_ready_) {
      std::vector<ibmw::abc::LatencyRecord> records;
      records.reserve(unique_);
      for (size_t i = 0; i < seen_.size(); ++i) {
        if (seen_[i]) records.push_back(latency_records_[i]);
      }
      artifact_ok_ = ibmw::abc::WriteLatencyArtifact(
          jsonl_, records, run_id_, producer_pid_, producer_start_ticks_,
          g_options.topic,
          valid && records.size() == g_options.count);
      if (!artifact_ok_)
        std::cerr << "Failed to finalize strict latency artifact\n";
    }
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
  ibmw::common::util::LatencyStats stats_;
  std::vector<bool> seen_;
  std::vector<ibmw::abc::LatencyRecord> latency_records_;
  std::ofstream jsonl_;
  std::string run_id_;
  uint64_t producer_pid_{0};
  uint64_t producer_start_ticks_{0};
  bool artifact_ready_{true};
  bool artifact_ok_{true};
};

// SIGINT/SIGTERM only write the signal number to a pipe (async-signal-safe);
// a watcher thread calls Engine::Shutdown(), which logs and locks.
std::mutex g_core_mutex;
int g_signal_pipe[2] = {-1, -1};

// Makes `core` the target of SIGINT/SIGTERM while in scope.
class ScopedSignalCore {
 public:
  explicit ScopedSignalCore(Engine* core) { Set(core); }
  ~ScopedSignalCore() { Set(nullptr); }
  ScopedSignalCore(const ScopedSignalCore&) = delete;
  ScopedSignalCore& operator=(const ScopedSignalCore&) = delete;

 private:
  static void Set(Engine* core) {
    std::lock_guard<std::mutex> lck(g_core_mutex);
    g_engine = core;
  }
};

void SignalHandler(int sig) {
  const int saved_errno = errno;
  const unsigned char byte = static_cast<unsigned char>(sig);
  [[maybe_unused]] auto ret = write(g_signal_pipe[1], &byte, 1);
  errno = saved_errno;
}

bool InstallSignalHandlers() {
  if (pipe2(g_signal_pipe, O_CLOEXEC) != 0 ||
      fcntl(g_signal_pipe[1], F_SETFL, O_NONBLOCK) != 0) {
    return false;
  }
  std::thread([] {
    unsigned char sig = 0;
    while (true) {
      const ssize_t n = read(g_signal_pipe[0], &sig, 1);
      if (n != 1) {
        if (n < 0 && errno == EINTR) continue;
        return;
      }
      std::lock_guard<std::mutex> lck(g_core_mutex);
      if (g_engine == nullptr) {
        // No engine (anymore): die from the signal.
        std::signal(sig, SIG_DFL);
        raise(sig);
        continue;
      }
      g_engine->Shutdown();
    }
  }).detach();
  return std::signal(SIGINT, SignalHandler) != SIG_ERR &&
         std::signal(SIGTERM, SignalHandler) != SIG_ERR;
}
}  // namespace

int main(int argc, char** argv) {
  if (!Parse(argc, argv)) { Usage(argv[0]); return 2; }
  if (!InstallSignalHandlers()) {
    std::cerr << "Failed to set up SIGINT/SIGTERM handling." << std::endl;
    return 1;
  }
  try {
    ibmw::runtime::core::Engine engine;
    PublisherModule publisher;
    SubscriberModule subscriber;
    if (g_options.role == "sub" && !subscriber.ArtifactReady()) return 1;
    ScopedSignalCore signal_core(&engine);
    if (g_options.role == "pub")
      engine.GetModuleBus().RegisterModule(publisher.NativeHandle());
    else
      engine.GetModuleBus().RegisterModule(subscriber.NativeHandle());
    ibmw::runtime::core::Engine::Options options;
    options.cfg_file_path = g_options.config;
    engine.Initialize(options);
    engine.Start();
    engine.Shutdown();
    if (g_options.role == "pub" && !publisher.PublisherOk()) return 1;
    if (g_options.role == "sub" && !subscriber.ArtifactOk()) return 1;
  } catch (const std::exception& error) {
    std::cerr << "B endpoint exception: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
