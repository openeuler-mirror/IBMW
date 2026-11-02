// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "topology_common.h"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/u_int8_multi_array.hpp"
using namespace std::chrono_literals;
namespace {
constexpr char kRaw[] = "/mw_perf/topology/raw";
constexpr char kDerived[] = "/mw_perf/topology/derived";
void Status(const ibmw::topology::Options& o, uint64_t received, uint64_t unique, uint64_t dup,
            uint64_t order, uint64_t bad, bool valid) {
  if (o.status.empty()) return;
  std::ofstream f(o.status);
  f << "{\"schema\":\"mw_perf.topology.status.v1\",\"role\":\"" << o.role << "\",\"sink_id\":\""
    << o.sink_id << "\",\"attempted_total\":" << o.warmup + o.count
    << ",\"attempted_measured\":" << o.count << ",\"received_measured\":" << received
    << ",\"unique_measured\":" << unique
    << ",\"missing\":" << o.count - std::min<uint64_t>(o.count, unique) << ",\"duplicate\":" << dup
    << ",\"out_of_order\":" << order << ",\"invalid_payload\":" << bad
    << ",\"invalid_timestamp\":0,\"valid\":" << (valid ? "true" : "false") << "}\n";
}
int Sink(const ibmw::topology::Options& o, std::shared_ptr<rclcpp::Node> n,
         rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr* keep) {
  auto qos = rclcpp::QoS(rclcpp::KeepLast(32)).reliable().durability_volatile();
  ibmw::common::util::LatencyStats stats("topology_ros2_" + o.sink_id);
  stats.Reserve(o.count);
  std::vector<bool> seen(o.count);
  std::vector<ibmw::topology::Record> records;
  records.reserve(o.count);
  uint64_t received = 0, unique = 0, dup = 0, order = 0, bad = 0, last = 0;
  bool have = false;
  auto sub = n->create_subscription<std_msgs::msg::UInt8MultiArray>(
      kDerived, qos, [&](std_msgs::msg::UInt8MultiArray::ConstSharedPtr m) {
        uint64_t seq = 0, send = 0, relay = 0;
        auto now = ibmw::common::util::MonotonicNs();
        if (!ibmw::topology::Valid(*m, now, &seq, &send, &relay)) {
          bad++;
          return;
        }
        if (seq < o.warmup || seq >= o.warmup + o.count) return;
        received++;
        if (have && seq <= last) order++;
        last = seq;
        have = true;
        auto i = seq - o.warmup;
        if (seen[i]) {
          dup++;
          return;
        }
        seen[i] = true;
        unique++;
        stats.Add(now - send);
        records.push_back({seq, send, relay, now});
        if (unique == o.count) rclcpp::shutdown();
      });
  *keep = sub;
  rclcpp::spin(n);
  uint64_t pid = 0, ticks = 0;
  const char *ep = getenv("MW_PERF_PRODUCER_PID"),
             *et = getenv("MW_PERF_PRODUCER_START_TIME_TICKS"), *run = getenv("MW_PERF_RUN_ID");
  try {
    if (ep) pid = std::stoull(ep);
    if (et) ticks = std::stoull(et);
  } catch (const std::exception& x) {
    // pid/ticks stay 0, so WriteArtifact fails and the run is reported invalid.
    std::cerr << "invalid MW_PERF_PRODUCER_* value: " << x.what() << "\n";
  }
  bool valid = unique == o.count && received == o.count && !dup && !order && !bad;
  std::ofstream jf;
  if (!o.jsonl.empty()) jf.open(o.jsonl);
  bool artifact =
      o.jsonl.empty() || ibmw::topology::WriteArtifact(jf, records, run ? run : "", "ROS2", "sink",
                                                       o.sink_id, pid, ticks, kDerived, valid);
  Status(o, received, unique, dup, order, bad, valid && artifact);
  if (valid && !o.csv.empty()) {
    std::ofstream f(o.csv, std::ios::app);
    std::ifstream e(o.csv, std::ios::ate);
    if (!e || e.tellg() == 0) f << ibmw::common::util::LatencyStats::kCsvHeader;
    stats.AppendCsvRow(f);
  }
  return valid && artifact ? 0 : 1;
}
}  // namespace
int main(int argc, char** argv) {
  ibmw::topology::Options o;
  if (!ibmw::topology::Parse(argc, argv, o, false)) {
    std::cerr << "usage: --role source|relay|sink --sink-id A|B [--csv PATH --status PATH --jsonl "
                 "PATH --hz N --warmup-messages N --message-count N]\n";
    return 2;
  }
  rclcpp::init(argc, argv);
  auto n = std::make_shared<rclcpp::Node>("ibmw_topology_ros2_endpoint");
  auto qos = rclcpp::QoS(rclcpp::KeepLast(32)).reliable().durability_volatile();
  if (o.role == "sink") {
    rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr keep;
    int r = Sink(o, n, &keep);
    rclcpp::shutdown();
    return r;
  }
  if (o.role == "relay") {
    auto pub = n->create_publisher<std_msgs::msg::UInt8MultiArray>(kDerived, qos);
    auto sub = n->create_subscription<std_msgs::msg::UInt8MultiArray>(
        kRaw, qos, [pub](std_msgs::msg::UInt8MultiArray::ConstSharedPtr in) {
          uint64_t s, t, r;
          if (ibmw::topology::Decode(*in, &s, &t, &r)) {
            auto out = *in;
            r = ibmw::common::util::MonotonicNs();
            std::memcpy(out.data.data() + 16, &r, 8);
            pub->publish(out);
          }
        });
    (void)sub;
    auto deadline = std::chrono::steady_clock::now() + 10s;
    while (rclcpp::ok() && pub->get_subscription_count() < 2 &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(10ms);
    if (pub->get_subscription_count() < 2) {
      std::cerr << "relay match timeout\n";
      rclcpp::shutdown();
      return 1;
    }
    rclcpp::spin(n);
    rclcpp::shutdown();
    return 0;
  }
  auto pub = n->create_publisher<std_msgs::msg::UInt8MultiArray>(kRaw, qos);
  auto deadline = std::chrono::steady_clock::now() + 10s;
  while (rclcpp::ok() && pub->get_subscription_count() == 0 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  if (pub->get_subscription_count() == 0) {
    std::cerr << "source match timeout\n";
    rclcpp::shutdown();
    return 1;
  }
  auto next = std::chrono::steady_clock::now();
  auto period = std::chrono::nanoseconds(1000000000ULL / o.hz);
  for (uint64_t s = 0; rclcpp::ok() && s < o.warmup + o.count; s++) {
    std_msgs::msg::UInt8MultiArray m;
    ibmw::topology::Make(m, s, ibmw::common::util::MonotonicNs());
    pub->publish(m);
    next += period;
    std::this_thread::sleep_until(next);
  }
  Status(o, 0, 0, 0, 0, 0, true);
  rclcpp::shutdown();
  return 0;
}
