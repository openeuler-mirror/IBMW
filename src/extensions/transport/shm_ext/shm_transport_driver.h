// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <functional>
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
  // drop-in replacement: the bus matches drivers by Name() and user loans
  // target Slot(), so both follow the name.
  static constexpr const char* kStableDriverId = "shm";
  explicit ShmTransportDriver(ShmBus& bus, std::string name = kStableDriverId)
      : bus_(bus), name_(std::move(name)), slot_(::ibmw::DriverSlotFromString(name_.c_str())) {}
  ~ShmTransportDriver() override = default;

  std::string_view Name() const noexcept override { return name_; }
  ibmw_driver_slot_t Slot() const noexcept override { return slot_; }

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

 private:
  enum class State : uint32_t { kPreInit, kInit, kStart, kShutdown };

  struct PodPublisherEndpoint {
    ShmPublisher* publisher;
    size_t initial_size_hint;
    size_t alignment;
  };

  bool LoanCtxMeta(ShmPublisher* pub, const runtime::core::transport::TransportInfo& info, size_t requested_size,
                   const ibmw::transport::Context& ctx, runtime::core::transport::LoanedSampleHandle& out_handle);
  bool LoanSerialized(ShmPublisher* pub, const runtime::core::transport::TransportInfo& info,
                      const ibmw::transport::Context& ctx, const void* msg_ptr,
                      runtime::core::transport::LoanedSampleHandle& out_handle);

  ShmBus& bus_;
  const std::string name_;
  const ibmw_driver_slot_t slot_;
  Options options_;
  std::atomic<State> state_ = State::kPreInit;
  mutable std::mutex lifecycle_mutex_;
  bool shutdown_started_ = false;
  const runtime::core::transport::TransportCatalog* transport_catalog_ptr_ = nullptr;

  std::unordered_map<std::string, std::shared_ptr<runtime::core::transport::MessageHandlerTool>>
      message_handler_map_;
  // Key "<topic>|<msg_type>"; frozen after Init, read lock-free afterwards.
  std::unordered_map<std::string, PodPublisherEndpoint> pod_pub_map_;
};

}  // namespace ibmw::extensions::shm_extension
