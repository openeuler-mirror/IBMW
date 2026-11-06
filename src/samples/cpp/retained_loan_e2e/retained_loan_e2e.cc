// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <array>
#include <atomic>
#include <csignal>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "c/util/type_support_base.h"
#include "cpp/transport/transport_context.h"
#include "cpp/transport/loaned_sample.h"
#include "cpp/transport/loaned_subscriber.h"
#include "engine/transport/transport_bus.h"
#ifdef IBMW_RETAINED_E2E_SHM
// shm_retained_loan_e2e: the same workloads on the self-developed shm driver
// (no RouDi; IBMW_SHM_ROOT / IBMW_SHM_DOMAIN select the discovery root).
#include "shm_ext/shm_bus.h"
#include "shm_ext/shm_transport_driver.h"
#else
#include "iceoryx_ext/iceoryx_bus.h"
#include "iceoryx_ext/iceoryx_transport_driver.h"
#endif
#ifdef IBMW_ENABLE_CORE_METRICS
#include "observability/core_metrics.h"
#endif

namespace {

std::atomic<bool> managed_stop{false};
void ManagedSignal(int) { managed_stop.store(true); }
void WaitForCollectorStop() {
  if (std::getenv("MW_PERF_MANAGED") == nullptr) return;
  std::signal(SIGTERM, ManagedSignal);
  std::signal(SIGINT, ManagedSignal);
  while (!managed_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

using namespace std::chrono_literals;
namespace core_transport = ibmw::runtime::core::transport;
#ifdef IBMW_RETAINED_E2E_SHM
namespace shm_ext = ibmw::extensions::shm_extension;
#else
namespace iox_ext = ibmw::extensions::iceoryx_extension;
#endif

constexpr std::string_view kTopic = "/ibmw/e2e/frame_bundle";
constexpr std::string_view kMessageType = "ibmw.realsense/FrameBundleV1";
constexpr uint32_t kMagic = 0x49424D57U;
constexpr uint32_t kVersion = 1U;
constexpr size_t kPublishCount = 240U;

struct SerializedPayload {
  std::vector<uint8_t> bytes;
};

const ibmw_type_support_base_t* DirectTypeSupport() {
  static const ibmw_string_view_t serializations[] = {{"cdr", 3}};
  static const ibmw_type_support_base_t support{
      .type_name = [](void*) { return ibmw_string_view_t{"ibmw.e2e/SerializedPayload", 26}; },
      .create = [](void*) -> void* { return new SerializedPayload; },
      .destroy = [](void*, void* value) { delete static_cast<SerializedPayload*>(value); },
      .copy = [](void*, const void* from, void* to) { *static_cast<SerializedPayload*>(to) = *static_cast<const SerializedPayload*>(from); },
      .move = [](void*, void* from, void* to) { *static_cast<SerializedPayload*>(to) = std::move(*static_cast<SerializedPayload*>(from)); },
      .serialize = [](void*, ibmw_string_view_t, const void* value,
                      const ibmw_buffer_array_allocator_t* allocator,
                      ibmw_buffer_array_t* buffers) {
        const auto& payload = *static_cast<const SerializedPayload*>(value);
        auto buffer = allocator->allocate(allocator->ctx, buffers, payload.bytes.size());
        if (buffer.data == nullptr || buffer.len < payload.bytes.size()) return false;
        std::memcpy(buffer.data, payload.bytes.data(), payload.bytes.size());
        buffers->data[buffers->len - 1].len = payload.bytes.size();
        return true;
      },
      .deserialize = [](void*, ibmw_string_view_t, ibmw_buffer_array_view_t, void*) { return false; },
      .serialization_types_supported_num = [](void*) { return size_t{1}; },
      .serialization_types_supported_list = [](void*) { return serializations; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &support;
}

struct alignas(64) FrameBundle {
  uint32_t magic;
  uint32_t version;
  uint64_t sequence;
  uint32_t payload_size;
  std::array<uint8_t, 256> payload;
  uint32_t checksum;
};

static_assert(std::is_trivially_copyable_v<FrameBundle>);
static_assert((alignof(FrameBundle) & (alignof(FrameBundle) - 1U)) == 0U);

uint32_t Checksum(const FrameBundle& frame) noexcept {
  uint32_t value = static_cast<uint32_t>(frame.sequence) ^
                   static_cast<uint32_t>(frame.sequence >> 32U) ^
                   frame.magic ^ frame.version ^ frame.payload_size;
  for (size_t i = 0; i < frame.payload_size; ++i) {
    value = (value * 16777619U) ^ frame.payload[i];
  }
  return value;
}

FrameBundle MakeFrame(uint64_t sequence) noexcept {
  FrameBundle frame{};
  frame.magic = kMagic;
  frame.version = kVersion;
  frame.sequence = sequence;
  frame.payload_size = static_cast<uint32_t>(frame.payload.size());
  for (size_t i = 0; i < frame.payload.size(); ++i) {
    frame.payload[i] = static_cast<uint8_t>((sequence + i) & 0xFFU);
  }
  frame.checksum = Checksum(frame);
  return frame;
}

bool IsValid(const FrameBundle& frame, size_t size) noexcept {
  return size == sizeof(FrameBundle) && frame.magic == kMagic &&
         frame.version == kVersion && frame.payload_size == frame.payload.size() &&
         frame.checksum == Checksum(frame);
}

YAML::Node MakeConfig(bool publisher) {
  YAML::Node root;
  YAML::Node driver;
#ifdef IBMW_RETAINED_E2E_SHM
  driver["type"] = "shm";
#else
  driver["type"] = "iceoryx";
#endif
  driver["options"] = YAML::Node(YAML::NodeType::Map);
  root["drivers"].push_back(driver);

  YAML::Node topic;
  topic["topic"] = std::string(kTopic);
#ifdef IBMW_RETAINED_E2E_SHM
  topic["enable_drivers"].push_back("shm");
#else
  topic["enable_drivers"].push_back("iceoryx");
#endif
  topic["enable_filters"] = YAML::Node(YAML::NodeType::Sequence);
  root[publisher ? "publishers" : "subscribers"].push_back(topic);
  return root;
}

struct Runtime {
  explicit Runtime(std::string_view runtime_id, bool publisher) {
#ifdef IBMW_ENABLE_CORE_METRICS
    ibmw::runtime::core::observability::CoreMetricsController().Initialize(this);
#endif
#ifdef IBMW_RETAINED_E2E_SHM
    shm_ext::ShmBusOptions bus_options;
    bus_options.runtime_name = std::string(runtime_id);
    shm_bus.Initialize(bus_options);
    auto concrete_driver =
        std::make_unique<shm_ext::ShmTransportDriver>(shm_bus, "shm");
#else
    iceoryx_bus.Initialize(sizeof(FrameBundle), runtime_id);
    auto concrete_driver =
        std::make_unique<iox_ext::IceoryxTransportDriver>(iceoryx_bus);
#endif
    auto* concrete_ptr = concrete_driver.get();
    driver = concrete_ptr;
    std::unique_ptr<core_transport::TransportDriver> driver =
        std::move(concrete_driver);
    transport_bus.RegisterTransportDriver(
        std::move(driver),
        core_transport::TransportDriverCapabilitiesV1{
            .shutdown_v1 = static_cast<
                core_transport::TransportShutdownCapabilityV1*>(concrete_ptr),
            .loaned_pod_registration_v1 = static_cast<
                core_transport::LoanedPodRegistrationCapabilityV1*>(
                concrete_ptr),
            .retained_loan_v1 = static_cast<
                core_transport::RetainedLoanCapabilityV1*>(concrete_ptr)});
    transport_bus.Initialize(MakeConfig(publisher));
  }

  ~Runtime() {
    if (!shutdown) Shutdown();
  }

  void Shutdown() {
    transport_bus.Shutdown();
#ifdef IBMW_ENABLE_CORE_METRICS
    if (!ibmw::runtime::core::observability::CoreMetricsController()
             .FinalizeSnapshot(this)) {
      std::cerr << "METRICS_FINAL_SNAPSHOT_FAIL\n";
    }
#endif
    shutdown = true;
  }

  void Start() {
    transport_bus.Start();
#ifdef IBMW_ENABLE_CORE_METRICS
    ibmw::runtime::core::observability::CoreMetricsController().Start(this);
#endif
  }

#ifdef IBMW_RETAINED_E2E_SHM
  shm_ext::ShmBus shm_bus;
  core_transport::TransportBus transport_bus;
  shm_ext::ShmTransportDriver* driver = nullptr;
#else
  iox_ext::IceoryxBus iceoryx_bus;
  core_transport::TransportBus transport_bus;
  iox_ext::IceoryxTransportDriver* driver = nullptr;
#endif
  bool shutdown = false;
};

int RunPublisher() {
  Runtime runtime("retained_e2e_pub", true);
  const auto& access = runtime.transport_bus.GetBusAccess("retained_e2e_pub");
  ibmw::transport::TransportRef transport(access.NativeHandle());
  auto sender = transport.GetPublisher(kTopic);
  if (!sender.RegisterLoanedPodTopic(kMessageType, sizeof(FrameBundle),
                                     alignof(FrameBundle))) {
    std::cerr << "RETAINED_E2E_PUBLISHER_FAIL reason=register\n";
    return 2;
  }

  runtime.Start();
  std::cout << "RETAINED_E2E_PUBLISHER_READY size=" << sizeof(FrameBundle)
            << " alignment=" << alignof(FrameBundle) << std::endl;

  size_t published = 0;
  size_t loan_failures = 0;
  for (uint64_t sequence = 1; sequence <= kPublishCount; ++sequence) {
    auto sample = ibmw::transport::LoanedSample<FrameBundle>::Borrow(
        sender, kMessageType);
    if (!sample.is_valid() || sample.size_bytes() != sizeof(FrameBundle)) {
      ++loan_failures;
      std::this_thread::sleep_for(25ms);
      continue;
    }
    *sample = MakeFrame(sequence);
    if (ibmw::transport::LoanedSample<FrameBundle>::Publish(
            sender, std::move(sample))) {
      ++published;
    }
    // Force a queue-empty edge after the retained drop window. Older
    // iceoryx listeners only re-notify after data transitions back to ready.
    std::this_thread::sleep_for(sequence == 25 ? 1s : 25ms);
  }

  runtime.Shutdown();
  std::cout << "RETAINED_E2E_PUBLISHER_DONE published=" << published
            << " loan_failures=" << loan_failures << std::endl;
  return published == 0 ? 3 : 0;
}

int RunDirectLoanWorkload() {
  constexpr std::string_view topic = "/ibmw/e2e/direct_serialized";
  Runtime runtime("direct_loan_metrics_owner", true);
  const auto* support = DirectTypeSupport();
  core_transport::TransportInfo info{
      .msg_type = "ibmw.e2e/SerializedPayload", .topic_name = std::string(topic),
      .msg_type_support_ref = ibmw::util::TypeSupportRef(support)};
  core_transport::SendTypeWrapper registration{.info = info};
  auto* driver = runtime.driver;
  if (driver == nullptr || !driver->RegisterSendType(registration)) {
    std::cerr << "DIRECT_LOAN_FAIL reason=register\n";
    return 2;
  }
  runtime.Start();

  SerializedPayload payload;
  payload.bytes.resize(20000U, 0x5aU);  // forces real 4K -> 8K -> 16K -> 32K retries
  ibmw::transport::Context context;
  context.SetSerializationType("cdr");
  core_transport::LoanedSampleHandle publish_handle{};
  core_transport::LoanedSampleHandle release_handle{};
  const bool publish_loan = driver->LoanSerializedSample(
      info, context, &payload, publish_handle);
  if (publish_loan) driver->PublishLoaned(publish_handle);
  const bool release_loan = driver->LoanSerializedSample(
      info, context, &payload, release_handle);
  if (release_loan) driver->ReleaseLoaned(release_handle);

  // A semantically invalid wire serialization name fails after the Reporter
  // attempt slot is incremented, exercising the terminal-failure path.
  ibmw::transport::Context invalid_context;
  invalid_context.SetSerializationType(std::string(256U, 'x'));
  core_transport::LoanedSampleHandle failed_handle{};
  const bool expected_failure = !driver->LoanSerializedSample(
      info, invalid_context, &payload, failed_handle);
  WaitForCollectorStop();
  runtime.Shutdown();
  const bool pass = publish_loan && release_loan && expected_failure;
  std::cout << "DIRECT_LOAN_WORKLOAD attempt=3 success=2 fail=1 "
               "publish=1 release=1 pass=" << pass << std::endl;
  return pass ? 0 : 3;
}

struct SubscriberState {
  std::mutex mutex;
  std::condition_variable cv;
  std::optional<ibmw::transport::ReceivedLoan> held_loan;
  std::thread::id callback_thread;
  size_t valid_callbacks = 0;
  size_t invalid_callbacks = 0;
  size_t callbacks_before_release = 0;
  uint64_t held_sequence = 0;
  std::atomic_bool released{false};
  std::atomic_bool cross_thread_release{false};
  std::atomic_bool held_payload_valid{false};
  std::atomic_size_t callbacks_after_release{0};
};

int RunSubscriber() {
  Runtime runtime("retained_e2e_sub", false);
  const auto& access = runtime.transport_bus.GetBusAccess("retained_e2e_sub");
  ibmw::transport::TransportRef transport(access.NativeHandle());
  auto receiver = transport.GetSubscriber(kTopic);

  SubscriberState state;
  ibmw::transport::LoanedSubscription subscription;
  const bool attached =
      ibmw::transport::LoanedSubscriber<FrameBundle>::AttachLeaseBytes(
          receiver, kMessageType,
          [&state](const FrameBundle& frame, size_t size,
                   ibmw::transport::ReceivedLoan loan) {
            std::lock_guard lock(state.mutex);
            if (!IsValid(frame, size)) {
              ++state.invalid_callbacks;
              return;
            }
            ++state.valid_callbacks;
            if (!state.held_loan.has_value() && !state.released.load()) {
              state.callback_thread = std::this_thread::get_id();
              state.held_sequence = frame.sequence;
              state.held_loan.emplace(std::move(loan));
#ifdef IBMW_RETAINED_E2E_SHM
              // The main thread and the releaser wait on this cv. notify_one
              // may wake only the main thread; the releaser then holds the
              // loan until its 7 s timeout, close to the publisher's end, so
              // recovery after the release is not guaranteed.
              state.cv.notify_all();
#else
              state.cv.notify_one();
#endif
            } else if (state.released.load()) {
              ++state.callbacks_after_release;
              state.cv.notify_one();
            }
          },
          &subscription, ibmw::transport::RetainedLoanOptions{1});
  if (!attached) {
    std::cerr << "RETAINED_E2E_SUBSCRIBER_FAIL reason=attach\n";
    return 2;
  }

  std::thread releaser([&state] {
    std::unique_lock lock(state.mutex);
    if (!state.cv.wait_for(lock, 7s,
                           [&state] { return state.held_loan.has_value(); })) {
      return;
    }
    lock.unlock();
    std::this_thread::sleep_for(750ms);
    lock.lock();
    state.callbacks_before_release = state.valid_callbacks;
    auto loan = std::move(*state.held_loan);
    state.held_loan.reset();
    const auto* held_frame = static_cast<const FrameBundle*>(loan.Data());
    state.held_payload_valid =
        held_frame != nullptr && held_frame->sequence == state.held_sequence &&
        IsValid(*held_frame, loan.Size());
    state.cross_thread_release =
        state.callback_thread != std::this_thread::get_id();
    state.released = true;
    lock.unlock();
    loan.Release();
    state.cv.notify_one();
    std::cout << "RETAINED_E2E_CROSS_THREAD_RELEASE ok="
              << state.cross_thread_release.load()
              << " payload_valid=" << state.held_payload_valid.load()
              << std::endl;
  });

  runtime.Start();
  std::cout << "RETAINED_E2E_SUBSCRIBER_READY max_outstanding=1" << std::endl;

  {
    std::unique_lock lock(state.mutex);
    state.cv.wait_for(lock, 10s, [&state] {
      return state.callbacks_after_release.load() > 0;
    });
  }
  releaser.join();

  WaitForCollectorStop();
  const bool close_ok = subscription.Close(3s);
  subscription.Reset();
  runtime.Shutdown();

  const bool drop_window = state.callbacks_before_release == 1;
  const bool recovery = state.callbacks_after_release.load() > 0;
  std::cout << "RETAINED_E2E_DROP_WINDOW ok=" << drop_window
            << " callbacks_before_release=" << state.callbacks_before_release
            << std::endl;
  std::cout << "RETAINED_E2E_RECOVERY ok=" << recovery
            << " callbacks_after_release=" << state.callbacks_after_release.load()
            << std::endl;

  const bool pass = close_ok && state.invalid_callbacks == 0 &&
                    state.valid_callbacks >= 2 && state.released.load() &&
                    state.cross_thread_release.load() &&
                    state.held_payload_valid.load() && drop_window && recovery;
  if (!pass) {
    std::cerr << "RETAINED_E2E_SUBSCRIBER_FAIL close=" << close_ok
              << " valid=" << state.valid_callbacks
              << " invalid=" << state.invalid_callbacks
              << " released=" << state.released.load() << std::endl;
    return 3;
  }

  std::cout << "RETAINED_E2E_SUBSCRIBER_PASS valid=" << state.valid_callbacks
            << " invalid=" << state.invalid_callbacks << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: retained_loan_e2e publisher|subscriber|direct\n";
    return 1;
  }
  if (std::string_view(argv[1]) == "publisher") return RunPublisher();
  if (std::string_view(argv[1]) == "subscriber") return RunSubscriber();
  if (std::string_view(argv[1]) == "direct") return RunDirectLoanWorkload();
  std::cerr << "usage: retained_loan_e2e publisher|subscriber|direct\n";
  return 1;
}
