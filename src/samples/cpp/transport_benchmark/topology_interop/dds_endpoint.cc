// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "topology_common.h"
#include "uint8_multi_array_serializer.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include "cpp/core.h"
#include "cpp/module_base.h"
#include "dds_ext/dds_transport_driver.h"
#include "dds_ext/ros2_type_helper.h"
#include "engine/engine.h"
#include "type_support/ros2/ros2_transport.h"
namespace {
using M = std_msgs::msg::UInt8MultiArray;
constexpr char raw[] = "/mw_perf/topology/raw";
constexpr char derived[] = "/mw_perf/topology/derived";
constexpr char type[] = "std_msgs/msg/UInt8MultiArray";
ibmw::topology::Options o;
ibmw::runtime::core::Engine* engine = nullptr;
bool Touch(const char* path) {
  if (!path || !*path) return true;
  std::ofstream f(path);
  return f.good();
}
bool WaitForStartGate() {
  const char* gate = std::getenv("MW_PERF_START_GATE");
  if (!gate || !*gate) return true;
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < end) {
    std::ifstream f(gate);
    if (f.good()) {
      std::cerr << "DDS start gate opened: " << gate << "\n";
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::ifstream f(gate);
  std::cerr << "DDS start gate timeout: path=" << gate
            << " exists=" << (f.good() ? "true" : "false") << "\n";
  return false;
}
bool Wait(ibmw::extensions::dds_extension::DdsTransportDriver* d, const std::string& t,
          int expected) {
  if (!d) return false;
  auto dt = ibmw::extensions::dds_extension::ROS2TypeHelper::ConvertToDdsTopicName(
      t, d->GetOptions().ros2_topic_prefix);
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  int final = 0;
  while (std::chrono::steady_clock::now() < end) {
    final = d->GetPublicationMatchedCount(dt);
    if (final >= expected) {
      std::cerr << "DDS publication match: topic=" << dt << " expected=" << expected
                << " final=" << final << "\n";
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  final = d->GetPublicationMatchedCount(dt);
  std::cerr << "DDS publication match timeout: topic=" << dt << " expected=" << expected
            << " final=" << final << "\n";
  return false;
}
class Source final : public ibmw::Module {
 public:
  ibmw::ModuleDescriptor Info() const override { return {.module_name = "TopologyDdsSource"}; }
  bool Setup(ibmw::Runtime c) override {
    s_ = c.GetTransport().GetPublisher(raw);
    return ibmw::ros2::transport::RegisterSendType<M>(s_, type);
  }
  bool Run() override {
    run_ = true;
    th_ = std::thread([this] {
      if (!Wait(d_, raw, 1)) {
        ok_ = false;
        engine->Shutdown();
        return;
      }
      const char* ready = std::getenv("MW_PERF_READY_FILE");
      if (ready && !Touch(ready)) {
        std::cerr << "DDS ready file create failed: " << ready << "\n";
        ok_ = false;
        engine->Shutdown();
        return;
      }
      if (!WaitForStartGate()) {
        ok_ = false;
        engine->Shutdown();
        return;
      }
      auto p = std::chrono::nanoseconds(1000000000ULL / o.hz);
      auto n = std::chrono::steady_clock::now();
      for (uint64_t i = 0; run_ && i < o.warmup + o.count; i++) {
        M m;
        ibmw::topology::Make(m, i, ibmw::common::util::MonotonicNs());
        ibmw::ros2::transport::Send(s_, type, m);
        n += p;
        std::this_thread::sleep_until(n);
      }
      engine->Shutdown();
    });
    return true;
  }
  void Stop() override {
    run_ = false;
    if (th_.joinable()) th_.join();
  }
  bool ok() const { return ok_; }

 private:
  ibmw::transport::SenderRef s_;
  ibmw::extensions::dds_extension::DdsTransportDriver* d_{nullptr};
  std::atomic<bool> run_{false}, ok_{true};
  std::thread th_;
  IBMW_USE_DRIVER(ibmw::extensions::dds_extension::DdsTransportDriver, d_);
};
class Relay final : public ibmw::Module {
 public:
  ibmw::ModuleDescriptor Info() const override { return {.module_name = "TopologyDdsRelay"}; }
  bool Setup(ibmw::Runtime c) override {
    s_ = c.GetTransport().GetPublisher(derived);
    auto r = c.GetTransport().GetSubscriber(raw);
    if (!ibmw::ros2::transport::RegisterSendType<M>(s_, type)) return false;
    return ibmw::ros2::transport::OnMessage<M>(
        r, type, [this](ibmw::transport::ContextRef, const std::shared_ptr<const M>& m) {
          uint64_t q, t, rn;
          if (ibmw::topology::Decode(*m, &q, &t, &rn)) {
            auto x = *m;
            rn = ibmw::common::util::MonotonicNs();
            std::memcpy(x.data.data() + 16, &rn, 8);
            ibmw::ros2::transport::Send(s_, type, x);
          }
        });
  }
  bool Run() override {
    if (!Wait(d_, derived, 2)) return false;
    const char* ready = std::getenv("MW_PERF_READY_FILE");
    return !ready || Touch(ready);
  }
  void Stop() override {}

 private:
  ibmw::transport::SenderRef s_;
  ibmw::extensions::dds_extension::DdsTransportDriver* d_{nullptr};
  IBMW_USE_DRIVER(ibmw::extensions::dds_extension::DdsTransportDriver, d_);
};
class Sink final : public ibmw::Module {
 public:
  Sink() : stats_("topology_dds_" + o.sink_id), seen_(o.count) { stats_.Reserve(o.count); }
  ibmw::ModuleDescriptor Info() const override { return {.module_name = "TopologyDdsSink"}; }
  bool Setup(ibmw::Runtime c) override {
    auto r = c.GetTransport().GetSubscriber(derived);
    return ibmw::ros2::transport::OnMessage<M>(
        r, type, [this](ibmw::transport::ContextRef, const std::shared_ptr<const M>& m) {
          uint64_t q, t, rn;
          auto now = ibmw::common::util::MonotonicNs();
          std::lock_guard l(mu_);
          if (!ibmw::topology::Decode(*m, &q, &t, &rn)) {
            bad_++;
            return;
          }
          if (!ibmw::topology::Valid(*m, now, &q, &t, &rn)) {
            invalid_timestamp_++;
            return;
          }
          if (q < o.warmup || q >= o.warmup + o.count) return;
          received_++;
          if (have_ && q <= last_) out_of_order_++;
          last_ = q;
          have_ = true;
          auto i = q - o.warmup;
          if (seen_[i]) {
            dup_++;
            return;
          }
          seen_[i] = true;
          unique_++;
          stats_.Add(now - t);
          records_.push_back({q, t, rn, now});
          if (unique_ == o.count) engine->Shutdown();
        });
  }
  bool Run() override { return true; }
  void Stop() override {
    std::lock_guard l(mu_);
    uint64_t pid = 0, ticks = 0;
    try {
      if (auto x = getenv("MW_PERF_PRODUCER_PID")) pid = std::stoull(x);
      if (auto x = getenv("MW_PERF_PRODUCER_START_TIME_TICKS")) ticks = std::stoull(x);
    } catch (...) {
    }
    auto run = getenv("MW_PERF_RUN_ID");
    bool valid = unique_ == o.count && received_ == o.count && !dup_ && !out_of_order_ && !bad_ &&
                 !invalid_timestamp_;
    std::ofstream j;
    if (!o.jsonl.empty()) j.open(o.jsonl);
    bool a =
        o.jsonl.empty() || ibmw::topology::WriteArtifact(j, records_, run ? run : "", "DDS", "sink",
                                                         o.sink_id, pid, ticks, derived, valid);
    if (!o.status.empty()) {
      std::ofstream f(o.status);
      f << "{\"schema\":\"mw_perf.topology.status.v1\",\"received_measured\":" << received_
        << ",\"unique_measured\":" << unique_
        << ",\"missing\":" << o.count - std::min<uint64_t>(o.count, unique_)
        << ",\"duplicate\":" << dup_ << ",\"out_of_order\":" << out_of_order_
        << ",\"invalid_payload\":" << bad_ << ",\"invalid_timestamp\":" << invalid_timestamp_
        << ",\"valid\":" << (valid && a ? "true" : "false") << "}\n";
    }
    if (valid && a && !o.csv.empty()) {
      std::ofstream f(o.csv, std::ios::app);
      std::ifstream e(o.csv, std::ios::ate);
      if (!e || e.tellg() == 0) f << ibmw::common::util::LatencyStats::kCsvHeader;
      stats_.AppendCsvRow(f);
    }
  }

 private:
  std::mutex mu_;
  uint64_t received_ = 0, unique_ = 0, dup_ = 0, out_of_order_ = 0, bad_ = 0,
           invalid_timestamp_ = 0, last_ = 0;
  bool have_ = false;
  std::vector<bool> seen_;
  std::vector<ibmw::topology::Record> records_;
  ibmw::common::util::LatencyStats stats_;
};
void Sig(int) {
  if (engine) engine->Shutdown();
}
}  // namespace
int main(int ac, char** av) {
  if (!ibmw::topology::Parse(ac, av, o, true)) return 2;
  signal(SIGINT, Sig);
  signal(SIGTERM, Sig);
  try {
    ibmw::runtime::core::Engine e;
    engine = &e;
    Source s;
    Relay r;
    Sink k;
    if (o.role == "source")
      e.GetModuleBus().RegisterModule(s.NativeHandle());
    else if (o.role == "relay")
      e.GetModuleBus().RegisterModule(r.NativeHandle());
    else
      e.GetModuleBus().RegisterModule(k.NativeHandle());
    ibmw::runtime::core::Engine::Options x;
    x.cfg_file_path = o.config;
    e.Initialize(x);
    e.Start();
    e.Shutdown();
    engine = nullptr;
    return o.role == "source" && !s.ok();
  } catch (const std::exception& x) {
    std::cerr << x.what() << "\n";
    return 1;
  }
}
