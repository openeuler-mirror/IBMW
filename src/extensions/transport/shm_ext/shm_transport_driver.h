// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/transport/transport_context.h"
#include "engine/thread_tools.h"
#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_capabilities.h"
#include "engine/transport/transport_driver_tools.h"
#include "shm_ext/shm_bus.h"

namespace ibmw::extensions::shm_extension {

// Transport driver on the self-developed zero-copy shm library. Contract,
// URLs, wire format and YAML options are the same as the iceoryx driver, so
// a deployment switches by loading this extension instead (optionally under
// the driver name "iceoryx" to keep existing topic configuration).
// Differences: an encoded URL longer than shm::kMaxUrlLength (1024 bytes) is
// rejected at registration (iceoryx truncated it); payload alignment is at
// most 4096 and one loan at most 256 MB.
class IBMW_SHM_EXPORT ShmTransportDriver
    : public runtime::core::transport::TransportDriver,
      public runtime::core::transport::TransportShutdownCapabilityV1,
      public runtime::core::transport::LoanedPodRegistrationCapabilityV1,
      public runtime::core::transport::RetainedLoanCapabilityV1 {
 public:
  struct Options {
    std::string listener_thread_name;
    std::string listener_thread_sched_policy;
    std::vector<uint32_t> listener_thread_bind_cpu;
    std::string benchmark_profile;
    uint64_t subscriber_queue_capacity = 1;
    std::string publisher_too_slow_policy;
    std::string subscriber_queue_full_policy;
  };

  // Same rule as the iceoryx driver: either everything omitted (legacy
  // defaults) or exactly the core_metrics_e2e_lossless_v1 profile.
  static std::optional<ShmDeliveryOptions> ResolveDeliveryOptions(const Options& options) noexcept;

  // `name` is "shm" by default. Running under "iceoryx" makes the driver a
  // drop-in replacement for topic configuration: the bus routes by Name(). The
  // slot is the driver's C++ type, not its name: IBMW_USE_DRIVER(ShmTransportDriver)
  // must bind under every name, and a module typed for the iceoryx driver must
  // never be handed this object.
  IBMW_DECLARE_DRIVER_SLOT("shm")
  explicit ShmTransportDriver(ShmBus& bus, std::string name = kStableDriverId)
      : bus_(bus), name_(std::move(name)), bus_generation_(bus.Generation()) {}
  ~ShmTransportDriver() override = default;

  std::string_view Name() const noexcept override { return name_; }

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  bool CanShutdownFromCurrentThread() const noexcept override;
  bool TryPrepareShutdown(std::chrono::milliseconds timeout) noexcept override;
  void Shutdown() override;
  std::list<std::pair<std::string, std::string>> GenInitializationReport() const noexcept override;

  void SetTransportCatalog(const runtime::core::transport::TransportCatalog* transport_catalog_ptr) noexcept override {
    transport_catalog_ptr_ = transport_catalog_ptr;
  }

  bool RegisterSendType(const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept override;
  bool RegisterLoanedPodTopic(const runtime::core::transport::TransportInfo& info, size_t initial_size_hint,
                              size_t alignment) noexcept override;
  bool OnMessage(const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept override;
  void Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept override;

  bool AttachLoaned(const runtime::core::transport::LoanedMessageHandlerWrapper& message_handler) noexcept override;
  std::unique_ptr<runtime::core::transport::LoanedSubscriptionHandle> AttachLoanedRetained(
      const runtime::core::transport::LoanedMessageHandlerWrapper& message_handler,
      runtime::core::transport::RetainedLoanOptions options) noexcept override;

  bool LoanSample(const runtime::core::transport::TransportInfo& info, size_t requested_size,
                  runtime::core::transport::LoanMode mode, const ibmw::transport::Context* ctx,
                  runtime::core::transport::LoanedSampleHandle& out_handle) noexcept override;
  bool LoanSerializedSample(const runtime::core::transport::TransportInfo& info, const ibmw::transport::Context& ctx,
                            const void* msg_ptr,
                            runtime::core::transport::LoanedSampleHandle& out_handle) noexcept override;
  void PublishLoaned(runtime::core::transport::LoanedSampleHandle& handle) noexcept override;
  void ReleaseLoaned(runtime::core::transport::LoanedSampleHandle& handle) noexcept override;
  // PublishLoaned() that tells whether the chunk was published: false for a
  // handle of another driver or mode, a stale or duplicate token, or a chunk
  // the library refused. The handle is left untouched only when it is not one
  // of this driver's (as PublishLoaned()); otherwise it is cleared.
  bool TryPublishLoaned(runtime::core::transport::LoanedSampleHandle& handle) noexcept;

  // Zero-copy POD registration under "/transport_zc_pod/<topic>/<msg_type>".
  bool EnsurePodTopic(std::string_view topic, std::string_view msg_type, size_t initial_size_hint,
                      size_t alignment = shm::kDefaultPayloadAlignment) noexcept;
  // The callback must not keep the pointer: the chunk is released on return.
  using PodMessageCallback = std::function<void(const void* payload, size_t size)>;
  bool RegisterPodSubscriber(std::string_view topic, std::string_view msg_type, PodMessageCallback callback) noexcept;

  // ctx-meta frame header, identical to the iceoryx driver:
  //   pkg_size(u32) | ser_type(u8 len + bytes) | ctx_num(u8) |
  //   (key u16 len + bytes | val u16 len + bytes)* | payload
  static size_t ComputeCtxMetaHeaderSize(std::string_view serialization_type,
                                         const std::unordered_map<std::string_view, std::string_view>& kv) noexcept;
  static size_t EncodeCtxMetaHeader(void* buffer, size_t buffer_size, uint32_t total_size,
                                    std::string_view serialization_type,
                                    const std::unordered_map<std::string_view, std::string_view>& kv) noexcept;

  ShmBus& Bus() noexcept { return bus_; }

  // Send() calls that failed, and how many of those were not logged: at most
  // one failure is logged per second. Shutdown refusals do not count.
  uint64_t SendFailures() const noexcept { return send_failures_.load(std::memory_order_relaxed); }
  uint64_t SendFailureLogsSuppressed() const noexcept {
    return send_failure_logs_suppressed_.load(std::memory_order_relaxed);
  }

 private:
  enum class State : uint32_t { kPreInit, kInit, kStart, kShutdown };

  struct PodPublisherEndpoint {
    ShmPublisher* publisher;
    size_t initial_size_hint;
    size_t alignment;
  };

  using PublisherMap = std::unordered_map<std::string, ShmPublisher*, ibmw::common::util::StringHash, std::equal_to<>>;

  // Publisher of a standard topic, nullptr when unknown. Requires a lease.
  ShmPublisher* FindStdPublisher(const ShmOperationGate::Lease& lease, std::string_view topic,
                                 std::string_view msg_type) const;
  void RecordSendFailure(const char* what) noexcept;

  bool LoanCtxMeta(ShmPublisher* pub, const runtime::core::transport::TransportInfo& info, size_t requested_size,
                   const ibmw::transport::Context& ctx, runtime::core::transport::LoanedSampleHandle& out_handle);
  bool LoanSerialized(ShmPublisher* pub, const runtime::core::transport::TransportInfo& info,
                      const ibmw::transport::Context& ctx, const void* msg_ptr,
                      runtime::core::transport::LoanedSampleHandle& out_handle);

  ShmBus& bus_;
  const std::string name_;
  const uint64_t bus_generation_;
  Options options_;
  std::atomic<State> state_ = State::kPreInit;
  mutable std::mutex lifecycle_mutex_;
  bool shutdown_started_ = false;
  // Start() attached every subscription; until then a Start() may be retried.
  bool delivery_started_ = false;
  const runtime::core::transport::TransportCatalog* transport_catalog_ptr_ = nullptr;

  std::unordered_map<std::string, std::shared_ptr<runtime::core::transport::MessageHandlerTool>>
      message_handler_map_;
  // Key: see TopicKey(). Both maps are written in Init state under
  // lifecycle_mutex_ only, then frozen: the seq_cst store of kStart in Start()
  // publishes them to every thread that reads kStart. The publishers they point
  // at live until ShmBus::Shutdown(), which requires the gate to be idle, so a
  // lease of the bus generation this driver was built for keeps them valid.
  std::unordered_map<std::string, PodPublisherEndpoint, ibmw::common::util::StringHash, std::equal_to<>> pod_pub_map_;
  PublisherMap std_pub_map_;

  std::atomic_uint64_t send_failures_{0};
  std::atomic_uint64_t send_failure_logs_suppressed_{0};
  std::atomic_int64_t last_send_failure_log_ns_{std::numeric_limits<int64_t>::min() / 2};
};

}  // namespace ibmw::extensions::shm_extension
