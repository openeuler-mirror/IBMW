// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "c/util/function_base.h"
#include "engine/transport/loaned_sample_handle.h"
#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_capabilities.h"
#include "engine/transport/transport_framework_async_filter.h"
#include "engine/transport/transport_catalog.h"
#include "base/log_util.h"

namespace ibmw::runtime::core::transport {

struct RegisterSendTypeAccessInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;

  const ibmw_type_support_base_t* msg_type_support;
};

struct RegisterLoanedPodAccessInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;
  ibmw_string_view_t msg_type;
  size_t initial_size_hint;
  size_t alignment;
};

struct SendAccessInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;

  ibmw_string_view_t msg_type;
  const ibmw_transport_context_base_t* ctx_ptr;
  const void* msg_ptr;
};

// Phase 7 §7.3 §K-3.3: dispatcher-level Loan op input.
//
// Carries the same identification quad as SendAccessInfoWrapper plus the
// requested loan size and wire-format mode. The dispatcher uses the quad
// to look up the registered SendTypeWrapper (which in turn carries the
// cached driver vector populated at RegisterSendType time), then asks
// the first driver whose LoanSample() returns true to fill in the
// returned handle. The hot path therefore intentionally bypasses the
// per-message ctx-meta KV writes (pub_seq / pub_ts) and the publish
// filter chain — Loan() is the user-level zero-copy fast lane and
// trades observability hooks for raw throughput. See NOTES §7.3 §F.
struct LoanAccessInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;

  ibmw_string_view_t msg_type;
  size_t requested_size;
  LoanMode mode;
};

struct MessageHandlerAccessInfoWrapper {
  std::string_view pkg_path;
  std::string_view module_name;
  std::string_view topic_name;

  // Non-owning. As with the framework's ordinary type registration APIs,
  // type support must have static registration lifetime and outlive dispatcher
  // teardown (including disabled legacy-driver tombstones).
  const ibmw_type_support_base_t* msg_type_support;
  ibmw_function_base_t* callback;
};

class TransportDriverDispatcher {
 public:
  using PublishTap = std::function<void(MessageFrame&)>;

  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 public:
  TransportDriverDispatcher()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~TransportDriverDispatcher() = default;

  TransportDriverDispatcher(const TransportDriverDispatcher&) = delete;
  TransportDriverDispatcher& operator=(const TransportDriverDispatcher&) = delete;

  void Initialize();
  void Start();
  void Shutdown();

  State GetState() const { return state_.load(); }

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  void SetTransportCatalog(TransportCatalog* transport_catalog_ptr);

  void SetPublishFiltersRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);
  void SetSubscribeFiltersRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  void SetPublishFrameworkAsyncTransportFilterBus(FrameworkAsyncTransportFilterBus* ptr);
  void SetSubscribeFrameworkAsyncTransportFilterBus(FrameworkAsyncTransportFilterBus* ptr);

  void SetPubTopicsDriversRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);
  void SetSubTopicsDriversRules(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  void RegisterTransportDriver(TransportDriver* transport_driver_ptr);
  void RegisterTransportDriver(
      TransportDriver* transport_driver_ptr,
      TransportDriverCapabilitiesV1 capabilities);

  // for access
  bool OnMessage(MessageHandlerAccessInfoWrapper&& wrapper);
  bool RegisterSendType(RegisterSendTypeAccessInfoWrapper&& wrapper);
  bool RegisterLoanedPodTopic(RegisterLoanedPodAccessInfoWrapper&& wrapper);
  void Send(SendAccessInfoWrapper&& wrapper);

  // Phase 7 §7.3 §U: receiver-side zero-copy fast lane (counterpart of
  // Loan() below). Iterates the sub-topic driver list and asks each
  // driver's AttachLoaned() in turn until one returns true. Returns
  // false if no driver supports zero-copy receive for this topic+type
  // (e.g. only dds is bound to the topic). This zero-copy API does not
  // silently fall back to OnMessage().
  bool AttachLoaned(MessageHandlerAccessInfoWrapper&& wrapper);
  std::unique_ptr<LoanedSubscriptionHandle> AttachLoanedRetained(
      MessageHandlerAccessInfoWrapper&& wrapper,
      RetainedLoanOptions options);

  // Phase 7 §7.3 §K-3.3: user-level zero-copy fast lane.
  //
  // Loan() returns an empty handle (user_payload_ptr == nullptr) on any
  // failure: state mismatch, unregistered type, no driver supports loan
  // for this (topic, mode). Callers MUST check user_payload_ptr before
  // writing.
  //
  // PublishLoaned() / ReleaseLoaned() route through handle.source_driver
  // populated by the issuing driver. Both clear the handle on completion,
  // and are no-ops on an empty/already-consumed handle.
  LoanedSampleHandle Loan(LoanAccessInfoWrapper&& wrapper);
  void PublishLoaned(LoanedSampleHandle& handle);
  void ReleaseLoaned(LoanedSampleHandle& handle);

  // for framework
  bool OnMessage(MessageHandlerWrapper&& wrapper);
  bool RegisterSendType(SendTypeWrapper&& wrapper);
  bool RegisterSendType(
      SendTypeWrapper&& wrapper,
      const std::vector<TransportDriver*>& drivers);
  void Send(MessageFrame&& wrapper);

  // Publish taps observe each ordinary Send() after the topic's filters have
  // forwarded it and before any transport driver is called. Taps run
  // synchronously on the publishing thread, once per logical publish and in
  // registration order. Exceptions are logged and do not suppress later taps
  // or driver delivery. The MessageFrame and every view reachable from it are
  // valid only for the callback; a tap retaining data must make an owned copy.
  //
  // UnregisterPublishTap() prevents new callback entries and, when called from
  // outside that tap, waits for callbacks already in flight. Self-unregister
  // and dispatcher Shutdown() from inside a callback wait only for invocations
  // on other threads, so they cannot wait for the current callback to return.
  // Callbacks may re-enter Send(), but should only enqueue bounded work because
  // their latency is part of the publisher hot path.
  //
  // The explicit Loan()/PublishLoaned() zero-copy fast path bypasses filters,
  // context metadata and publish taps by design.
  uint64_t RegisterPublishTap(PublishTap tap);
  bool UnregisterPublishTap(uint64_t handle);

  using TopicDriverInfoMap = std::unordered_map<std::string_view, std::vector<std::string_view>>;
  TopicDriverInfoMap GetPubTopicDriverInfo() const;
  TopicDriverInfoMap GetSubTopicDriverInfo() const;

 private:
  std::vector<TransportDriver*> GetDriversByRules(
      std::string_view topic_name,
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  std::vector<std::string> GetFilterRules(
      std::string_view topic_name,
      const std::vector<std::pair<std::string, std::vector<std::string>>>& rules);

  struct PublishTapState;
  void InvokePublishTaps(MessageFrame& frame) noexcept;
  void DeactivatePublishTap(
      const std::shared_ptr<PublishTapState>& tap_state);

 private:
  std::atomic<State> state_ = State::kPreInit;
  uint64_t sub_topic_index_ = 0;
  uint64_t pub_topic_index_ = 0;

  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  TransportCatalog* transport_catalog_ptr_;

  // filter
  std::vector<std::pair<std::string, std::vector<std::string>>> publish_filters_rules_;
  std::vector<std::pair<std::string, std::vector<std::string>>> subscribe_filters_rules_;

  FrameworkAsyncTransportFilterBus* publish_filter_bus_ptr_ = nullptr;
  FrameworkAsyncTransportFilterBus* subscribe_filter_bus_ptr_ = nullptr;

  // driver
  std::vector<TransportDriver*> transport_driver_index_vec_;
  std::unordered_map<TransportDriver*, TransportDriverCapabilitiesV1>
      transport_driver_capabilities_map_;

  std::vector<std::pair<std::string, std::vector<std::string>>> pub_topics_drivers_rules_;
  std::vector<std::pair<std::string, std::vector<std::string>>> sub_topics_drivers_rules_;

  std::unordered_map<
      std::string,
      std::vector<TransportDriver*>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      pub_topics_driver_index_map_;

  std::unordered_map<
      std::string,
      std::vector<TransportDriver*>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      sub_topics_driver_index_map_;

  std::unordered_map<
      std::string,
      std::atomic<uint32_t>,
      ibmw::common::util::StringHash,
      std::equal_to<>>
      pub_topic_seq_map_;

  struct PodRegistration {
    TransportInfo info;
    size_t initial_size_hint;
    size_t alignment;
    TransportDriver* driver;
  };
  std::unordered_map<std::string, std::unique_ptr<PodRegistration>>
      pod_registration_map_;

  // Legacy OnMessage has no unregister operation. Drivers may retain wrapper
  // addresses even when registration fails, so release these only after every
  // driver has completed Shutdown().
  std::vector<std::unique_ptr<MessageHandlerWrapper>>
      owned_legacy_message_handler_pool_;

  std::mutex publish_taps_mutex_;
  struct PublishTapState {
    struct InvocationToken {
      PublishTapState* state;
      InvocationToken* previous;
    };

    explicit PublishTapState(PublishTap input_tap)
        : tap(std::move(input_tap)) {}

    static InvocationToken*& CurrentInvocation() {
      static thread_local InvocationToken* current = nullptr;
      return current;
    }
    size_t CurrentDepth() const noexcept {
      size_t depth = 0;
      for (auto* call = CurrentInvocation(); call; call = call->previous) {
        if (call->state == this) ++depth;
      }
      return depth;
    }

    PublishTap tap;
    std::mutex mutex;
    std::condition_variable idle_cv;
    bool active = true;
    size_t in_flight = 0;
  };
  std::map<uint64_t, std::shared_ptr<PublishTapState>> publish_taps_;
  uint64_t next_publish_tap_handle_ = 1;
};

}  // namespace ibmw::runtime::core::transport
