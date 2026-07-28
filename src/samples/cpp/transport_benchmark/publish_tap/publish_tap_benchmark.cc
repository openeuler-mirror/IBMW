// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "base/benchmark_util.h"
#include "cpp/util/string.h"
#include "engine/transport/transport_driver_dispatcher.h"

// Allocation accounting is local to this benchmark executable.  Counting is
// enabled only for the measured Send loop, after all fixtures, payload slots
// and LatencyStats storage have been preallocated.
namespace {
std::atomic<bool> g_count_allocations{false};
std::atomic<uint64_t> g_allocation_count{0};

void CountAllocation() noexcept {
  if (g_count_allocations.load(std::memory_order_relaxed)) {
    g_allocation_count.fetch_add(1, std::memory_order_relaxed);
  }
}

void* Allocate(std::size_t size) {
  CountAllocation();
  if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
    return ptr;
  }
  throw std::bad_alloc();
}

void* AllocateAligned(std::size_t size, std::size_t alignment) {
  CountAllocation();
  void* ptr = nullptr;
  if (posix_memalign(&ptr, alignment, size == 0 ? 1 : size) == 0) {
    return ptr;
  }
  throw std::bad_alloc();
}
}  // namespace

void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::align_val_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept {
  std::free(ptr);
}
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
  std::free(ptr);
}
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
  std::free(ptr);
}

namespace ibmw::samples::transport_benchmark::publish_tap {
namespace {

using ibmw::runtime::core::transport::FrameworkAsyncTransportFilterBus;
using ibmw::runtime::core::transport::MessageFrame;
using ibmw::runtime::core::transport::SendAccessInfoWrapper;
using ibmw::runtime::core::transport::SendTypeWrapper;
using ibmw::runtime::core::transport::TransportCatalog;
using ibmw::runtime::core::transport::TransportDriver;
using ibmw::runtime::core::transport::TransportDriverDispatcher;
using ibmw::runtime::core::transport::TransportInfo;

constexpr std::string_view kMessageType = "PublishTapBenchmarkPayload";
constexpr std::string_view kTopic = "publish_tap_benchmark";
constexpr std::string_view kPackage = "benchmark";
constexpr std::string_view kModule = "publisher";

enum class Mode { kNoTap, kIdleTap, kOwnedCopyTap };

struct Options {
  size_t iterations = 50'000;
  size_t warmup = 5'000;
  size_t repeats = 5;
  size_t payload_bytes = 4096;
  size_t queue_depth = 1024;
};

std::string_view ModeName(Mode mode) {
  switch (mode) {
    case Mode::kNoTap:
      return "no_tap";
    case Mode::kIdleTap:
      return "idle_tap";
    case Mode::kOwnedCopyTap:
      return "owned_copy_tap";
  }
  return "unknown";
}

bool ParseSize(std::string_view text, size_t* result) {
  if (text.empty() || result == nullptr) return false;
  uint64_t parsed = 0;
  const auto converted =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
      parsed == 0 || parsed > std::numeric_limits<size_t>::max()) {
    return false;
  }
  *result = static_cast<size_t>(parsed);
  return true;
}

enum class ParseResult { kRun, kHelp, kError };

ParseResult ParseOptions(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help") {
      std::cout
          << "Usage: ibmw_publish_tap_benchmark [options]\n"
          << "  --iterations N    measured sends per repeat (default 50000)\n"
          << "  --warmup N        unmeasured sends per mode (default 5000)\n"
          << "  --repeats N       repeats per mode (default 5)\n"
          << "  --payload-bytes N copied bytes for owned tap (default 4096)\n"
          << "  --queue-depth N   bounded owned-copy slots (default 1024)\n";
      return ParseResult::kHelp;
    }
    if (index + 1 >= argc) {
      std::cerr << "Missing value for " << arg << '\n';
      return ParseResult::kError;
    }
    size_t* destination = nullptr;
    if (arg == "--iterations") destination = &options->iterations;
    if (arg == "--warmup") destination = &options->warmup;
    if (arg == "--repeats") destination = &options->repeats;
    if (arg == "--payload-bytes") destination = &options->payload_bytes;
    if (arg == "--queue-depth") destination = &options->queue_depth;
    if (destination == nullptr || !ParseSize(argv[++index], destination)) {
      std::cerr << "Invalid option or positive integer: " << arg << '\n';
      return ParseResult::kError;
    }
  }
  return ParseResult::kRun;
}

class CountingDriver final : public TransportDriver {
 public:
  std::string_view Name() const noexcept override { return "publish-tap-bench"; }
  IBMW_DECLARE_DRIVER_SLOT("ibmw.benchmark.publish-tap")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterSendType(const SendTypeWrapper&) noexcept override {
    return true;
  }
  bool OnMessage(
      const ibmw::runtime::core::transport::MessageHandlerWrapper&) noexcept
      override {
    return false;
  }
  void Send(MessageFrame& frame) noexcept override {
    ++send_count_;
    // Make delivery observable without adding work that allocates.
    if (frame.msg_ptr != nullptr) {
      checksum_ ^= static_cast<const unsigned char*>(frame.msg_ptr)[0];
    }
  }

  uint64_t SendCount() const noexcept { return send_count_; }
  uint8_t Checksum() const noexcept { return checksum_; }

 private:
  uint64_t send_count_ = 0;
  uint8_t checksum_ = 0;
};

// Recording-style bounded queue: every callback makes an owned payload copy
// into a preallocated ring.  Once full it overwrites the oldest slot and
// reports that pressure explicitly, matching a bounded/drop-oldest policy
// without adding allocator noise to the measured hot path.
class BoundedOwnedPayloadQueue {
 public:
  BoundedOwnedPayloadQueue(size_t depth, size_t payload_bytes)
      : payload_bytes_(payload_bytes), slots_(depth) {
    for (auto& slot : slots_) slot.resize(payload_bytes_);
  }

  void Capture(MessageFrame& frame) noexcept {
    if (frame.msg_ptr == nullptr) return;
    std::memcpy(slots_[next_slot_].data(), frame.msg_ptr, payload_bytes_);
    checksum_ ^= static_cast<unsigned char>(slots_[next_slot_].front());
    next_slot_ = (next_slot_ + 1) % slots_.size();
    if (captured_ >= slots_.size()) ++overwritten_;
    ++captured_;
  }

  uint64_t Captured() const noexcept { return captured_; }
  uint64_t Overwritten() const noexcept { return overwritten_; }
  uint8_t Checksum() const noexcept { return checksum_; }

 private:
  size_t payload_bytes_;
  std::vector<std::vector<std::byte>> slots_;
  size_t next_slot_ = 0;
  uint64_t captured_ = 0;
  uint64_t overwritten_ = 0;
  uint8_t checksum_ = 0;
};

class DispatcherFixture {
 public:
  DispatcherFixture(Mode mode, size_t queue_depth, size_t payload_bytes)
      : mode_(mode), owned_queue_(queue_depth, payload_bytes) {
    // Suppress the per-message trace logger in the benchmark hot path.  The
    // dispatcher still executes the same production code; only its diagnostic
    // sink is replaced so logging I/O cannot dominate latency samples.
    const auto quiet_logger =
        std::make_shared<ibmw::common::util::LoggerWrapper>(
            ibmw::common::util::LoggerWrapper{
                .get_log_level_func = []() -> uint32_t {
                  return ibmw::common::util::kLogLevelFatal + 1u;
                },
                .log_func = [](uint32_t, uint32_t, const char*, const char*,
                               const char*, size_t) {}});
    catalog_.SetLogger(quiet_logger);
    dispatcher_.SetLogger(quiet_logger);
    dispatcher_.SetTransportCatalog(&catalog_);
    dispatcher_.SetPublishFrameworkAsyncTransportFilterBus(&filters_);
    dispatcher_.SetPubTopicsDriversRules({{".*", {"publish-tap-bench"}}});
    dispatcher_.RegisterTransportDriver(&driver_);
    dispatcher_.Initialize();

    SendTypeWrapper wrapper;
    wrapper.info = info_;
    if (!dispatcher_.RegisterSendType(std::move(wrapper), {&driver_})) {
      throw std::runtime_error("failed to register benchmark send type");
    }

    if (mode_ == Mode::kIdleTap) {
      tap_handle_ = dispatcher_.RegisterPublishTap(
          [this](MessageFrame&) noexcept { ++tap_callbacks_; });
    } else if (mode_ == Mode::kOwnedCopyTap) {
      tap_handle_ = dispatcher_.RegisterPublishTap(
          [this](MessageFrame& frame) noexcept {
            ++tap_callbacks_;
            owned_queue_.Capture(frame);
          });
    }
    if (mode_ != Mode::kNoTap && tap_handle_ == 0) {
      throw std::runtime_error("failed to register benchmark publish tap");
    }
    dispatcher_.Start();
  }

  ~DispatcherFixture() {
    if (tap_handle_ != 0) dispatcher_.UnregisterPublishTap(tap_handle_);
    dispatcher_.Shutdown();
  }

  void Send(const void* payload) {
    context_.Reset();
    dispatcher_.Send(SendAccessInfoWrapper{
        .pkg_path = kPackage,
        .module_name = kModule,
        .topic_name = kTopic,
        .msg_type = ibmw::util::ToIbmwStringView(kMessageType),
        .ctx_ptr = context_.NativeHandle(),
        .msg_ptr = payload});
  }

  uint64_t SendCount() const noexcept { return driver_.SendCount(); }
  uint64_t TapCallbacks() const noexcept { return tap_callbacks_; }
  const BoundedOwnedPayloadQueue& OwnedQueue() const noexcept {
    return owned_queue_;
  }
  uint8_t ObservableChecksum() const noexcept {
    return static_cast<uint8_t>(driver_.Checksum() ^ owned_queue_.Checksum());
  }

 private:
  Mode mode_;
  TransportCatalog catalog_;
  FrameworkAsyncTransportFilterBus filters_;
  CountingDriver driver_;
  TransportDriverDispatcher dispatcher_;
  TransportInfo info_{.msg_type = std::string(kMessageType),
                      .topic_name = std::string(kTopic),
                      .pkg_path = std::string(kPackage),
                      .module_name = std::string(kModule),
                      .index = 1};
  ibmw::transport::Context context_;
  BoundedOwnedPayloadQueue owned_queue_;
  uint64_t tap_handle_ = 0;
  uint64_t tap_callbacks_ = 0;
};

struct Result {
  ibmw::common::util::LatencyStats::Summary latency;
  uint64_t allocations = 0;
  uint64_t tap_callbacks = 0;
  uint64_t overwritten = 0;
  uint8_t checksum = 0;
};

Result Run(Mode mode, const Options& options,
           const std::vector<std::byte>& payload) {
  DispatcherFixture fixture(mode, options.queue_depth, options.payload_bytes);
  for (size_t index = 0; index < options.warmup; ++index) {
    fixture.Send(payload.data());
  }

  ibmw::common::util::LatencyStats stats(std::string(ModeName(mode)));
  stats.Reserve(options.iterations);
  g_allocation_count.store(0, std::memory_order_relaxed);
  g_count_allocations.store(true, std::memory_order_relaxed);
  for (size_t index = 0; index < options.iterations; ++index) {
    const uint64_t begin = ibmw::common::util::MonotonicNs();
    fixture.Send(payload.data());
    stats.Add(ibmw::common::util::MonotonicNs() - begin);
  }
  g_count_allocations.store(false, std::memory_order_relaxed);

  const uint64_t expected = options.warmup + options.iterations;
  if (fixture.SendCount() != expected) {
    throw std::runtime_error("benchmark driver did not observe every Send");
  }
  if (mode == Mode::kNoTap && fixture.TapCallbacks() != 0) {
    throw std::runtime_error("no-tap mode unexpectedly invoked a tap");
  }
  if (mode != Mode::kNoTap && fixture.TapCallbacks() != expected) {
    throw std::runtime_error("registered tap did not observe every Send");
  }
  if (mode == Mode::kOwnedCopyTap &&
      fixture.OwnedQueue().Captured() != expected) {
    throw std::runtime_error("owned-copy tap did not capture every Send");
  }

  return Result{.latency = stats.Compute(),
                .allocations =
                    g_allocation_count.load(std::memory_order_relaxed),
                .tap_callbacks = fixture.TapCallbacks(),
                .overwritten = fixture.OwnedQueue().Overwritten(),
                .checksum = fixture.ObservableChecksum()};
}

}  // namespace
}  // namespace ibmw::samples::transport_benchmark::publish_tap

int main(int argc, char** argv) {
  using namespace ibmw::samples::transport_benchmark::publish_tap;
  Options options;
  const ParseResult parse_result = ParseOptions(argc, argv, &options);
  if (parse_result == ParseResult::kHelp) return 0;
  if (parse_result == ParseResult::kError) return 2;

  std::vector<std::byte> payload(options.payload_bytes, std::byte{0xA5});
  constexpr Mode kModes[]{Mode::kNoTap, Mode::kIdleTap,
                          Mode::kOwnedCopyTap};

  std::cout
      << "mode,repeat,iterations,payload_bytes,queue_depth,allocations,"
         "allocations_per_send,p50_us,p90_us,p99_us,p999_us,max_us,"
         "tap_callbacks,queue_overwrites,checksum\n";
  std::cout << std::fixed << std::setprecision(3);
  try {
    for (size_t repeat = 1; repeat <= options.repeats; ++repeat) {
      for (const Mode mode : kModes) {
        const Result result = Run(mode, options, payload);
        const auto& latency = result.latency;
        std::cout << ModeName(mode) << ',' << repeat << ','
                  << options.iterations << ',' << options.payload_bytes << ','
                  << options.queue_depth << ',' << result.allocations << ','
                  << static_cast<double>(result.allocations) /
                         static_cast<double>(options.iterations)
                  << ',' << latency.p50_ns / 1000.0 << ','
                  << latency.p90_ns / 1000.0 << ','
                  << latency.p99_ns / 1000.0 << ','
                  << latency.p999_ns / 1000.0 << ','
                  << latency.max_ns / 1000.0 << ',' << result.tap_callbacks
                  << ',' << result.overwritten << ','
                  << static_cast<unsigned>(result.checksum) << '\n';
      }
    }
  } catch (const std::exception& error) {
    g_count_allocations.store(false, std::memory_order_relaxed);
    std::cerr << "publish-tap benchmark failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
