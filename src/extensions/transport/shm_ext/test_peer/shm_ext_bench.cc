// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Microbenchmark of the shm_ext loan/publish hot path: in-process
// ShmTransportDriver instances set up like the fixture of
// shm_transport_driver_test.cc. Only uses API that exists since 89b5a1a, so it
// also builds against that tree for an A/B comparison; see
// selfdev/shm/docs/测试文档.md §6.5. A loan only gets its first 8 bytes written:
// the point is the path's cost, which a memset of the payload would hide.
//
// Usage: ibmw_shm_ext_bench [--reps N] [--ms N] [--cpus a,b,c,...]
// IBMW_SHM_ROOT must be set. Each worker thread is pinned to the next CPU of
// --cpus; listener threads keep the mask of the process.
//
// Prints one line per case, the median over the repetitions of a process:
//   RESULT case=<name> payload=<bytes> threads=<n> ns_per_op=<x> mops=<x> ...
// With several threads ns_per_op is per thread (n * 1000 / mops).
//
// Built with Reporter V1 (IBMW_SHM_EXT_BENCH_REPORTER_HOST, see CMakeLists.txt)
// the binary is also the Reporter V1 host: IBMW_CORE_METRICS=counters makes the
// compiled-in shm_ext sources count into its core metrics, and the final
// snapshot goes to IBMW_OBSERVABILITY_OUTPUT_DIR/metrics.jsonl (default ".").
// Prints METRICS_MODE=<counters|off> and METRICS_FINAL_SNAPSHOT=<ok|fail>.

#include <sched.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cpp/transport/transport_handle.h"
#include "cpp/util/buffer_array_allocator.h"
#include "shm_ext/global.h"
#include "shm_ext/shm_bus.h"
#include "shm_ext/shm_transport_driver.h"
#include "yaml-cpp/yaml.h"
#ifdef IBMW_SHM_EXT_BENCH_REPORTER_HOST
#include "observability/core_metrics.h"
#endif

namespace ibmw::extensions::shm_extension {
namespace {

using namespace std::chrono_literals;
namespace transport = runtime::core::transport;

constexpr char kSerType[] = "bench";
constexpr char kBlobType[] = "shm_ext_bench/Blob";
constexpr size_t kPayloads[] = {64, 4096};
constexpr int kThreadCounts[] = {1, 2, 4, 8};

struct Args {
  int reps = 5;
  int ms = 100;
  std::vector<int> cpus;
};
Args g_args;
std::atomic<uint64_t> g_pin_failures{0};

// ---------------------------------------------------------------- message type

struct BlobMsg {
  std::string data;
};

const ibmw_string_view_t* SerTypes(void*) {
  static const ibmw_string_view_t kTypes[] = {{kSerType, sizeof(kSerType) - 1}};
  return kTypes;
}
size_t SerTypesNum(void*) { return 1; }
const void* NoCustom(void*) { return nullptr; }
ibmw_string_view_t BlobName(void*) { return {kBlobType, sizeof(kBlobType) - 1}; }
void* BlobCreate(void*) { return new BlobMsg(); }
void BlobDestroy(void*, void* m) { delete static_cast<BlobMsg*>(m); }
void BlobCopy(void*, const void* from, void* to) { *static_cast<BlobMsg*>(to) = *static_cast<const BlobMsg*>(from); }
void BlobMove(void*, void* from, void* to) { *static_cast<BlobMsg*>(to) = std::move(*static_cast<BlobMsg*>(from)); }
bool BlobSerialize(void*, ibmw_string_view_t, const void* msg, const ibmw_buffer_array_allocator_t* allocator,
                   ibmw_buffer_array_t* buffer_array) {
  const auto& data = static_cast<const BlobMsg*>(msg)->data;
  auto b = allocator->allocate(allocator->ctx, buffer_array, data.size());
  if (b.data == nullptr || b.len != data.size()) return false;
  std::memcpy(b.data, data.data(), data.size());
  return true;
}
bool BlobDeserialize(void*, ibmw_string_view_t, ibmw_buffer_array_view_t, void*) { return false; }

const ibmw_type_support_base_t* BlobTypeSupport() {
  static const ibmw_type_support_base_t kSupport{
      .type_name = BlobName,
      .create = BlobCreate,
      .destroy = BlobDestroy,
      .copy = BlobCopy,
      .move = BlobMove,
      .serialize = BlobSerialize,
      .deserialize = BlobDeserialize,
      .serialization_types_supported_num = SerTypesNum,
      .serialization_types_supported_list = SerTypes,
      .custom_type_support_ptr = NoCustom,
      .ctx = nullptr,
  };
  return &kSupport;
}

transport::TransportInfo BlobInfo(const std::string& topic) {
  transport::TransportInfo info;
  info.msg_type = kBlobType;
  info.topic_name = topic;
  info.msg_type_support_ref = ibmw::util::TypeSupportRef(BlobTypeSupport());
  return info;
}

std::string Topic(const char* name, size_t payload, int index) {
  return std::string("b/") + name + "/" + std::to_string(payload) + "/" + std::to_string(index) + "/" +
         std::to_string(::getpid());
}

// A host installs a logger at its configured level; the fallback of an extension
// logs at Trace, and the MW_TRACE of every Send() would be what is measured.
ibmw_logger_base_t g_warn_logger{
    .get_log_level = [](void*) { return static_cast<ibmw_log_level_t>(common::util::kLogLevelWarn); },
    .log =
        [](void*, ibmw_log_level_t level, uint32_t line, const char* file_name, const char* function_name,
           const char* log_data, size_t log_data_size) {
          common::util::SimpleLogger::Log(static_cast<uint32_t>(level), line, file_name, function_name, log_data,
                                          log_data_size);
        },
    .ctx = nullptr,
};

// ------------------------------------------------------------------ measuring

void PinToCpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) g_pin_failures.fetch_add(1);
}

struct Rep {
  double mops = 0;  // aggregate over the threads
  uint64_t ops = 0;
  uint64_t fails = 0;
};

// Runs `op(thread_index)` on `threads` pinned threads for about `ms` ms. A false
// return is counted as a failure and not as an operation.
template <class Op>
Rep RunRep(int threads, Op& op) {
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<double> mops(threads);
  std::vector<uint64_t> ops(threads), fails(threads);
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      int cpu = -1;
      if (!g_args.cpus.empty()) {
        cpu = g_args.cpus[static_cast<size_t>(t) % g_args.cpus.size()];
        PinToCpu(cpu);
      }
      ready.fetch_add(1);
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      const auto start = std::chrono::steady_clock::now();
      const auto end = start + std::chrono::milliseconds(g_args.ms);
      uint64_t n = 0, f = 0;
      auto now = start;
      do {
        for (int i = 0; i < 256; ++i) {
          if (op(t)) {
            ++n;
          } else {
            ++f;
          }
        }
        now = std::chrono::steady_clock::now();
      } while (now < end);
      mops[t] = static_cast<double>(n) / std::chrono::duration<double, std::micro>(now - start).count();
      ops[t] = n;
      fails[t] = f;
      if (cpu >= 0 && ::sched_getcpu() != cpu) g_pin_failures.fetch_add(1);
    });
  }
  while (ready.load() != threads) std::this_thread::yield();
  go.store(true, std::memory_order_release);
  for (auto& w : workers) w.join();
  Rep rep;
  for (int t = 0; t < threads; ++t) {
    rep.mops += mops[t];
    rep.ops += ops[t];
    rep.fails += fails[t];
  }
  return rep;
}

double Median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 != 0 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// One discarded repetition (page faults, frequency ramp), then --reps.
template <class Op>
void Measure(const char* name, size_t payload, int threads, Op op, const std::function<uint64_t()>& extra_fails = {},
             const std::atomic<uint64_t>* delivered = nullptr) {
  (void)RunRep(threads, op);
  const uint64_t delivered_before = delivered != nullptr ? delivered->load() : 0;
  std::vector<double> mops;
  uint64_t ops = 0, fails = 0;
  for (int r = 0; r < g_args.reps; ++r) {
    const Rep rep = RunRep(threads, op);
    mops.push_back(rep.mops);
    ops += rep.ops;
    fails += rep.fails;
  }
  if (extra_fails) fails += extra_fails();
  const double m = Median(mops);
  const auto [lo, hi] = std::minmax_element(mops.begin(), mops.end());
  std::printf("RESULT case=%s payload=%zu threads=%d ns_per_op=%.2f mops=%.3f rep_min_mops=%.3f rep_max_mops=%.3f "
              "ops=%llu fails=%llu delivered=%llu\n",
              name, payload, threads, threads * 1000.0 / m, m, *lo, *hi, static_cast<unsigned long long>(ops),
              static_cast<unsigned long long>(fails),
              static_cast<unsigned long long>(delivered != nullptr ? delivered->load() - delivered_before : 0));
  std::fflush(stdout);
}

// ------------------------------------------------------------------- fixture

// Same order as ShmDriverTest::Init(): bus, then an initialized driver.
class Bench {
 public:
  Bench() {
    bus_ = std::make_unique<ShmBus>();
    bus_->Initialize(ShmBusOptions{});
    driver_ = std::make_unique<ShmTransportDriver>(*bus_);
    driver_->Initialize(YAML::Node());
  }
  ~Bench() {
    driver_->Shutdown();
    driver_.reset();
    bus_.reset();
  }
  Bench(const Bench&) = delete;
  Bench& operator=(const Bench&) = delete;

  ShmTransportDriver& Driver() { return *driver_; }
  ShmBus& Bus() { return *bus_; }

 private:
  std::unique_ptr<ShmBus> bus_;
  std::unique_ptr<ShmTransportDriver> driver_;
};

std::vector<transport::TransportInfo> PodTopics(ShmTransportDriver& d, const char* name, size_t payload, int count) {
  std::vector<transport::TransportInfo> infos;
  for (int i = 0; i < count; ++i) {
    infos.push_back(BlobInfo(Topic(name, payload, i)));
    if (!d.EnsurePodTopic(infos.back().topic_name, kBlobType, payload)) throw std::runtime_error("EnsurePodTopic");
  }
  return infos;
}

// A loan that is written to and comes back false when refused.
inline bool LoanPod(ShmTransportDriver& d, const transport::TransportInfo& info, size_t payload,
                    transport::LoanedSampleHandle& h) {
  if (!d.LoanSample(info, payload, transport::LoanMode::kPodOnly, nullptr, h)) return false;
  *static_cast<volatile uint64_t*>(h.user_payload_ptr) = 1;
  return true;
}

// ---------------------------------------------------------------------- cases

// Control: one acquire + release of the bus operation gate. The loan-return gate
// of d9f9c59 is the same class, and this gate exists in every tree. With several
// threads it shows what one more contended bus-wide gate costs by itself.
void GatePair(int threads) {
  Bench b;
  auto& d = b.Driver();
  auto& bus = b.Bus();
  d.Start();
  Measure(threads == 1 ? "gate_pair" : "scale_gate_pair", 0, threads, [&](int) {
    auto lease = bus.TryAcquireOperation();
    return static_cast<bool>(lease);
  });
}

void PodLoanRelease(size_t payload) {
  Bench b;
  auto& d = b.Driver();
  const auto infos = PodTopics(d, "release", payload, 1);
  d.Start();
  Measure("pod_loan_release", payload, 1, [&](int) {
    transport::LoanedSampleHandle h;
    if (!LoanPod(d, infos[0], payload, h)) return false;
    d.ReleaseLoaned(h);
    return true;
  });
}

void PodLoanPublish(size_t payload, int threads, const char* name) {
  Bench b;
  auto& d = b.Driver();
  const auto infos = PodTopics(d, name, payload, threads);
  d.Start();
  Measure(name, payload, threads, [&](int t) {
    transport::LoanedSampleHandle h;
    return LoanPod(d, infos[static_cast<size_t>(t)], payload, h) && d.TryPublishLoaned(h);
  });
}

// The default delivery never blocks the publisher: with queue capacity 1 a
// sample the listener has not taken yet is replaced, so `delivered` < ops.
void PodLoanPublishSubscriber(size_t payload) {
  Bench b;
  auto& d = b.Driver();
  const auto infos = PodTopics(d, "pub_sub", payload, 1);
  std::atomic<uint64_t> delivered{0};
  const std::string& topic = infos[0].topic_name;
  if (!d.RegisterPodSubscriber(topic, kBlobType, [&](const void*, size_t) {
        delivered.fetch_add(1, std::memory_order_relaxed);
      })) {
    throw std::runtime_error("RegisterPodSubscriber");
  }
  d.Start();
  // Publish until the subscriber has connected and taken a sample. Only API the
  // plugin exports, so ibmw_shm_ext_bench_so (linked against it) can do it too.
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (delivered.load() == 0) {
    if (std::chrono::steady_clock::now() > end) throw std::runtime_error("no subscriber on " + topic);
    transport::LoanedSampleHandle h;
    if (LoanPod(d, infos[0], payload, h)) d.TryPublishLoaned(h);
    std::this_thread::sleep_for(1ms);
  }
  Measure(
      "pod_loan_publish_sub", payload, 1,
      [&](int) {
        transport::LoanedSampleHandle h;
        return LoanPod(d, infos[0], payload, h) && d.TryPublishLoaned(h);
      },
      {}, &delivered);
  // The subscriber callback refers to `delivered`: stop it before that goes.
  d.Shutdown();
}

std::shared_ptr<ibmw::transport::Context> PubContext() {
  auto ctx = std::make_shared<ibmw::transport::Context>(
      ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT);
  ctx->SetSerializationType(kSerType);
  ctx->SetMetaValue("origin", "bench");
  return ctx;
}

transport::SendTypeWrapper SendType(const transport::TransportInfo& info) {
  transport::SendTypeWrapper w;
  w.info = info;
  return w;
}

void CtxLoanPublish(size_t payload) {
  Bench b;
  auto& d = b.Driver();
  const auto info = BlobInfo(Topic("ctx", payload, 0));
  if (!d.RegisterSendType(SendType(info))) throw std::runtime_error("RegisterSendType");
  d.Start();
  const auto ctx = PubContext();
  Measure("ctx_loan_publish", payload, 1, [&](int) {
    transport::LoanedSampleHandle h;
    if (!d.LoanSample(info, payload, transport::LoanMode::kCtxMetaThenPayload, ctx.get(), h)) return false;
    *static_cast<volatile uint64_t*>(h.user_payload_ptr) = 1;
    return d.TryPublishLoaned(h);
  });
}

// Control: the framed Send() path has no loan-return gate.
void SendSmall(size_t payload) {
  Bench b;
  auto& d = b.Driver();
  const auto info = BlobInfo(Topic("send", payload, 0));
  if (!d.RegisterSendType(SendType(info))) throw std::runtime_error("RegisterSendType");
  d.Start();
  const auto ctx = PubContext();
  BlobMsg msg{std::string(payload, 'x')};
  Measure(
      "send", payload, 1,
      [&](int) {
        transport::MessageFrame frame{.info = info, .msg_ptr = &msg, .ctx_ref = ctx};
        d.Send(frame);
        return true;
      },
      [&] { return d.SendFailures(); });
}

std::vector<int> ParseCpus(const std::string& list) {
  std::vector<int> cpus;
  size_t pos = 0;
  while (pos < list.size()) {
    size_t used = 0;
    cpus.push_back(std::stoi(list.substr(pos), &used));
    pos += used + 1;
  }
  return cpus;
}

int Main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) {
      std::fprintf(stderr, "usage: %s [--reps N] [--ms N] [--cpus a,b,c,...]\n", argv[0]);
      return 64;
    }
    if (a == "--reps") {
      g_args.reps = std::stoi(argv[++i]);
    } else if (a == "--ms") {
      g_args.ms = std::stoi(argv[++i]);
    } else if (a == "--cpus") {
      g_args.cpus = ParseCpus(argv[++i]);
    } else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 64;
    }
  }
  if (std::getenv("IBMW_SHM_ROOT") == nullptr) {
    std::fprintf(stderr, "IBMW_SHM_ROOT must be set\n");
    return 64;
  }
  ::unsetenv("IBMW_SHM_DOMAIN");
  ::signal(SIGPIPE, SIG_IGN);
#ifdef IBMW_SHM_EXT_BENCH_SO
  SetShmExtensionLogger(ibmw::logger::LoggerRef(&g_warn_logger));  // g_logger is not exported
#else
  g_logger.Set(ibmw::logger::LoggerRef(&g_warn_logger));  // also builds against older trees
#endif
#ifdef IBMW_SHM_EXT_BENCH_REPORTER_HOST
  // Initialized before the first driver, so every driver finds the host
  // reporter. Started (which freezes the registry) only after the last case:
  // each case registers its publishers after the previous case has ended, and a
  // frozen registry refuses new slots. Counting does not depend on Start.
  static const int kMetricsOwner = 0;
  auto& metrics = runtime::core::observability::CoreMetricsController();
  metrics.Initialize(&kMetricsOwner);
  std::printf("METRICS_MODE=%s\n", metrics.Enabled() ? "counters" : "off");
#endif

  {
    // Downcast a base pointer as shm_loaned_pod does. In ibmw_shm_ext_bench_so
    // the object and its vtable come from the .so, so this needs the exported
    // ShmTransportDriver typeinfo to be the one the exe compares against.
    Bench bench;
    runtime::core::transport::TransportDriver* base = &bench.Driver();
    asm volatile("" : "+r"(base));  // keep the compiler from folding the cast
    if (dynamic_cast<ShmTransportDriver*>(base) != &bench.Driver()) {
      throw std::runtime_error("dynamic_cast<ShmTransportDriver*> from TransportDriver* failed");
    }
    std::printf("DYNCAST_SHM_DRIVER=PASS\n");
  }

  for (int threads : kThreadCounts) GatePair(threads);
  for (size_t payload : kPayloads) {
    PodLoanRelease(payload);
    PodLoanPublish(payload, 1, "pod_loan_publish");
    PodLoanPublishSubscriber(payload);
    CtxLoanPublish(payload);
    SendSmall(payload);
    for (int threads : kThreadCounts) PodLoanPublish(payload, threads, "scale_pod_loan_publish");
  }
#ifdef IBMW_SHM_EXT_BENCH_REPORTER_HOST
  metrics.Start(&kMetricsOwner);
  const bool snapshot_ok = metrics.FinalizeSnapshot(&kMetricsOwner);
  std::printf("METRICS_FINAL_SNAPSHOT=%s\n", snapshot_ok ? "ok" : "fail");
#endif
  std::printf("PIN_FAILURES=%llu\n", static_cast<unsigned long long>(g_pin_failures.load()));
#ifdef IBMW_SHM_EXT_BENCH_REPORTER_HOST
  if (!snapshot_ok) return 1;
#endif
  return 0;
}

}  // namespace
}  // namespace ibmw::extensions::shm_extension

int main(int argc, char** argv) {
  try {
    return ibmw::extensions::shm_extension::Main(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bench failed: %s\n", e.what());
    return 1;
  }
}
