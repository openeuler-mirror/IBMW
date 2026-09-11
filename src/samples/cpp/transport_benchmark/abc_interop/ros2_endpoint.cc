#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/benchmark_util.h"
#include "ros2_latency_artifact.h"
#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/u_int8_multi_array.hpp"

namespace {
constexpr size_t kPayloadBytes = 4096;
constexpr uint8_t kFillerMagic = 0xA5;
constexpr char kDefaultTopic[] = "/mw_perf/payload_4kb";

struct Options {
  std::string topic{kDefaultTopic};
  uint32_t hz{100};
  uint32_t warmup{50};
  uint32_t count{3000};
  std::string csv;
  std::string status;
  std::string jsonl;
  std::string role;
};

bool Uint(const char* text, uint32_t* value) {
  try {
    size_t used = 0;
    auto parsed = std::stoul(text, &used);
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

bool Parse(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    if (a == "--help" || a == "-h") return false;
    if (i + 1 >= argc) return false;
    if (a == "--topic") o->topic = argv[++i];
    else if (a == "--hz") { if (!Uint(argv[++i], &o->hz)) return false; }
    else if (a == "--warmup-messages") {
      if (!Uint(argv[++i], &o->warmup)) return false;
    }
    else if (a == "--message-count") {
      if (!Uint(argv[++i], &o->count)) return false;
    }
    else if (a == "--csv") o->csv = argv[++i];
    else if (a == "--status") o->status = argv[++i];
    else if (a == "--jsonl") o->jsonl = argv[++i];
    else if (a == "--role") o->role = argv[++i];
    else return false;
  }
  return o->role == "pub" || o->role == "sub";
}

void Usage(const char* p) {
  std::cerr << "Usage: " << p << " --role pub|sub [--topic T] [--hz N] "
            << "[--warmup-messages N] [--message-count N] [--csv PATH] [--status PATH] [--jsonl PATH]\n";
}

void Put(std::vector<uint8_t>* data, uint64_t send_ns, uint64_t seq) {
  data->assign(kPayloadBytes, kFillerMagic);
  std::memcpy(data->data(), &send_ns, sizeof(send_ns));
  std::memcpy(data->data() + sizeof(send_ns), &seq, sizeof(seq));
}

int Run(const Options& o, int argc, char** argv) {
  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("ibmw_abc_ros2_endpoint");
  auto qos = rclcpp::QoS(rclcpp::KeepLast(8192)).reliable().durability_volatile();
  ibmw::common::util::LatencyStats stats("abc_ros2_4KB");
  stats.Reserve(o.count);
  uint64_t received = 0, unique = 0, duplicate = 0, out_of_order = 0;
  uint64_t invalid_payload = 0, invalid_timestamp = 0, last_seq = 0;
  bool have_last = false;
  std::vector<bool> seen(o.count, false);
  std::vector<ibmw::abc::LatencyRecord> latency_records(o.count);
  std::ofstream jsonl;
  std::string run_id;
  uint64_t producer_pid = 0;
  uint64_t producer_start_ticks = 0;
  bool artifact_ready = true;
  bool artifact_ok = true;
  if (o.role == "sub" && !o.jsonl.empty()) {
    const char* run = std::getenv("MW_PERF_RUN_ID");
    const char* schema = std::getenv("MW_PERF_ARTIFACT_SCHEMA_VERSION");
    const char* producer_pid_env = std::getenv("MW_PERF_PRODUCER_PID");
    const char* producer_start_ticks_env =
        std::getenv("MW_PERF_PRODUCER_START_TIME_TICKS");
    run_id = run ? run : "";
    artifact_ready = !run_id.empty() && schema && std::string(schema) == ibmw::abc::kLatencySchema &&
                     PositiveUint64(producer_pid_env, &producer_pid) &&
                     PositiveUint64(producer_start_ticks_env,
                                    &producer_start_ticks);
    jsonl.open(o.jsonl, std::ios::out | std::ios::trunc);
    artifact_ready = artifact_ready && static_cast<bool>(jsonl);
    if (!artifact_ready) { std::cerr << "Failed to initialize strict latency artifact\n"; rclcpp::shutdown(); }
  }

  if (o.role == "sub") {
    auto sub = node->create_subscription<std_msgs::msg::UInt8MultiArray>(o.topic, qos,
      [&](std_msgs::msg::UInt8MultiArray::ConstSharedPtr msg) {
        if (msg->data.size() != kPayloadBytes) { ++invalid_payload; return; }
        bool zero_filler = true, magic_filler = true;
        for (size_t i = 16; i < msg->data.size(); ++i) {
          zero_filler = zero_filler && msg->data[i] == 0;
          magic_filler = magic_filler && msg->data[i] == kFillerMagic;
        }
        if (!zero_filler && !magic_filler) { ++invalid_payload; return; }
        uint64_t send_ns = 0, seq = 0;
        std::memcpy(&send_ns, msg->data.data(), sizeof(send_ns));
        std::memcpy(&seq, msg->data.data() + sizeof(send_ns), sizeof(seq));
        const auto now = ibmw::common::util::MonotonicNs();
        if (seq < o.warmup || seq >= static_cast<uint64_t>(o.warmup) + o.count) return;
        ++received;
        if (send_ns == 0 || send_ns > now) { ++invalid_timestamp; return; }
        if (have_last && seq <= last_seq) ++out_of_order;
        last_seq = seq; have_last = true;
        const size_t index = static_cast<size_t>(seq - o.warmup);
        if (seen[index]) { ++duplicate; return; }
        seen[index] = true;
        stats.Add(now - send_ns);
        latency_records[index] = {seq, send_ns, now, msg->data.size()};
        unique = 0; for (bool value : seen) unique += value ? 1 : 0;
        if (unique == o.count && o.jsonl.empty()) rclcpp::shutdown();
      });
    (void)sub;
    rclcpp::spin(node);
  } else {
    auto pub = node->create_publisher<std_msgs::msg::UInt8MultiArray>(o.topic, qos);
    while (rclcpp::ok() && pub->get_subscription_count() == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto period = std::chrono::nanoseconds(1'000'000'000ULL / o.hz);
    auto next = std::chrono::steady_clock::now();
    for (uint64_t seq = 0; rclcpp::ok() && seq < o.warmup + o.count; ++seq) {
      std_msgs::msg::UInt8MultiArray msg;
      Put(&msg.data, ibmw::common::util::MonotonicNs(), seq);
      pub->publish(msg);
      next += period;
      std::this_thread::sleep_until(next);
    }
    rclcpp::shutdown();
  }
  if (o.role == "sub") {
    std::cout << stats.ToHumanString() << "\n";
    uint64_t unique = 0; for (bool v : seen) unique += v ? 1 : 0;
    const uint64_t missing = o.count > unique ? o.count - unique : 0;
    const bool valid = unique == o.count && received == o.count && duplicate == 0 &&
                       out_of_order == 0 && invalid_payload == 0 && invalid_timestamp == 0;
    if (!o.jsonl.empty() && artifact_ready) {
      std::vector<ibmw::abc::LatencyRecord> records;
      records.reserve(unique);
      for (size_t i = 0; i < seen.size(); ++i) if (seen[i]) records.push_back(latency_records[i]);
      artifact_ok = ibmw::abc::WriteLatencyArtifact(jsonl, records, run_id,
          producer_pid, producer_start_ticks, o.topic,
          valid && records.size() == o.count);
      if (!artifact_ok) std::cerr << "Failed to finalize strict latency artifact\n";
    }
    if (!o.status.empty()) {
      std::ofstream status(o.status, std::ios::trunc);
      status << "{\n  \"attempted_total\": " << (o.warmup + o.count)
             << ",\n  \"attempted_measured\": " << o.count
             << ",\n  \"received_measured\": " << received
             << ",\n  \"unique_measured\": " << unique
             << ",\n  \"missing\": " << missing << ",\n  \"duplicate\": " << duplicate
             << ",\n  \"out_of_order\": " << out_of_order
             << ",\n  \"invalid_payload\": " << invalid_payload
             << ",\n  \"invalid_timestamp\": " << invalid_timestamp
             << ",\n  \"valid\": " << (valid ? "true" : "false") << "\n}\n";
    }
    if (valid && !o.csv.empty()) {
      std::ofstream out(o.csv, std::ios::app);
      std::ifstream in(o.csv, std::ios::ate);
      if (in.tellg() == 0) out << ibmw::common::util::LatencyStats::kCsvHeader;
      stats.AppendCsvRow(out);
    }
  } else if (!o.status.empty()) {
    std::ofstream status(o.status, std::ios::trunc);
    status << "{\n  \"attempted_total\": " << (o.warmup + o.count)
           << ",\n  \"attempted_measured\": " << o.count << "\n}\n";
  }
  return (o.role == "sub" && !o.jsonl.empty() && (!artifact_ready || !artifact_ok)) ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  Options o;
  if (!Parse(argc, argv, &o)) { Usage(argv[0]); return 2; }
  return Run(o, argc, argv);
}
