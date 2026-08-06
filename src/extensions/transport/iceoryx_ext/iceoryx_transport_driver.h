// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <type_traits>

#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_capabilities.h"
#include "engine/transport/transport_driver_tools.h"
#include "engine/thread_tools.h"
#include "iceoryx_ext/iceoryx_bus.h"
#include "cpp/transport/transport_context.h"

// IBMW_ICEORYX_EXPORT - mark a symbol with default ELF visibility so it is
// part of the iceoryx_ext shared library's public ABI.
//
// Background: the iceoryx_ext target builds with CXX_VISIBILITY_PRESET=hidden
// (set globally by the top-level CMakeLists for size + dlopen hygiene), and
// only the IBMW_EXT_ENTRY symbol is exported by default. Sample/benchmark
// executables that link this .so directly (instead of going through the
// engine's dlopen path) need access to a small handful of symbols beyond the
// extension entry point -- specifically the IceoryxTransportDriver class
// (typeinfo + vtable + methods called by the header-only IoxStylePublisher /
// IoxRawPodPublisher / IoxLoanedSample templates).
//
// Marking the class itself triggers ELF visibility default for typeinfo,
// vtable AND every member function in one go (g++/clang behaviour for
// "type_visibility" promotion). This is preferred over annotating each
// method individually because it keeps RTTI consistent for dynamic_cast.
#ifndef IBMW_ICEORYX_EXPORT
#define IBMW_ICEORYX_EXPORT __attribute__((visibility("default")))
#endif

namespace ibmw::extensions::iceoryx_extension {

class IBMW_ICEORYX_EXPORT IceoryxTransportDriver
    : public runtime::core::transport::TransportDriver,
      public runtime::core::transport::TransportShutdownCapabilityV1,
      public runtime::core::transport::LoanedPodRegistrationCapabilityV1,
      public runtime::core::transport::RetainedLoanCapabilityV1 {
 public:
  struct Options {
    std::string listener_thread_name;
    std::string listener_thread_sched_policy;
    std::vector<uint32_t> listener_thread_bind_cpu;
  };

 public:
  IceoryxTransportDriver(IceoryxBus& iceoryx_bus)
      : iceoryx_bus_(iceoryx_bus) {}

  ~IceoryxTransportDriver() override = default;

  std::string_view Name() const noexcept override { return "iceoryx"; }
  IBMW_DECLARE_DRIVER_SLOT("iceoryx")

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  bool CanShutdownFromCurrentThread() const noexcept override;
  bool TryPrepareShutdown(std::chrono::milliseconds timeout) noexcept override;
  void Shutdown() override;

  void SetTransportCatalog(const runtime::core::transport::TransportCatalog* transport_catalog_ptr) noexcept override {
    transport_catalog_ptr_ = transport_catalog_ptr;
  }

  bool RegisterSendType(
      const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept override;
  bool RegisterLoanedPodTopic(
      const runtime::core::transport::TransportInfo& info,
      size_t initial_size_hint, size_t alignment) noexcept override;
  bool OnMessage(const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept override;
  void Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept override;

  // Phase 7 §7.3 §U: receiver-side zero-copy fast lane override.
  // Reuses the existing RegisterPodSubscriber() machinery (POD URL
  // prefix "/transport_zc_pod/<topic>/<msg_type>") under the hood: the
  // dispatcher hands us a LoanedMessageHandlerCallback, we wrap it in a
  // PodMessageCallback that drops ctx_ptr/release (iceoryx releases the
  // chunk when the user callback returns) and forwards payload+size.
  bool AttachLoaned(
      const runtime::core::transport::LoanedMessageHandlerWrapper& message_handler) noexcept override;
  std::unique_ptr<runtime::core::transport::LoanedSubscriptionHandle>
  AttachLoanedRetained(
      const runtime::core::transport::LoanedMessageHandlerWrapper& message_handler,
      runtime::core::transport::RetainedLoanOptions options) noexcept override;

  // ----- Zero-copy Loan API (kPodOnly + kCtxMetaThenPayload) -----
  // Phase 7 §X1: ctx parameter unifies the previously extension-only
  // LoanSampleWithCtx() into the base virtual function family. ctx is
  // required (non-null) for kCtxMetaThenPayload mode and ignored for
  // kPodOnly mode (callers may pass nullptr).
  bool LoanSample(const runtime::core::transport::TransportInfo& info,
                  size_t requested_size,
                  runtime::core::transport::LoanMode mode,
                  const ibmw::transport::Context* ctx,
                  runtime::core::transport::LoanedSampleHandle& out_handle) noexcept override;
  // Phase 7 §X1: promoted from extension-only LoanSerializedSampleWithCtx().
  bool LoanSerializedSample(const runtime::core::transport::TransportInfo& info,
                            const ibmw::transport::Context& ctx,
                            const void* msg_ptr,
                            runtime::core::transport::LoanedSampleHandle& out_handle) noexcept override;
  void PublishLoaned(runtime::core::transport::LoanedSampleHandle& handle) noexcept override;
  void ReleaseLoaned(runtime::core::transport::LoanedSampleHandle& handle) noexcept override;

  // ----- Extension-only API for zero-copy POD pub/sub registration -----
  // Lazily creates an iceoryx publisher under the URL prefix
  // "/transport_zc_pod/<topic>/<msg_type>". Idempotent.
  // Returns true if topic is now registered (or was already).
  bool EnsurePodTopic(std::string_view topic, std::string_view msg_type,
                      size_t initial_size_hint,
                      size_t alignment =
                          iox::CHUNK_DEFAULT_USER_PAYLOAD_ALIGNMENT) noexcept;

  // Register a POD subscriber. The callback receives a pointer into the
  // iceoryx SHM chunk and its size in bytes. The callback MUST NOT retain
  // the pointer beyond its own return (the chunk is released right after).
  using PodMessageCallback = std::function<void(const void* payload, size_t size)>;
  bool RegisterPodSubscriber(std::string_view topic, std::string_view msg_type,
                             PodMessageCallback callback) noexcept;

  // ----- Extension-only API (kept for benchmark baseline only;
  //        DO NOT USE in production code -- use LoanSample() with
  //        LoanMode::kCtxMetaThenPayload / LoanSerializedSample() instead.) -----
  //
  // ctx-meta mode reuses the standard "/transport/<topic>/<msg_type>" URL,
  // so any subscriber that goes through OnMessage() (i.e. the regular IBMW
  // pub/sub stack) can decode it transparently. The on-wire layout produced
  // here is identical to IceoryxTransportDriver::Send():
  //
  //   pkg_size(u32) | serialization_type(u8+bytes) |
  //   ctx_num(u8)   | (ctx_key u16+bytes | ctx_val u16+bytes)*N |
  //   user_payload(payload_size bytes)

  // ---- ctx-meta header layout helpers (public for unit testing) ----
  //
  // Compute the exact number of bytes the ctx-meta header occupies given a
  // serialization type and ctx-meta KV pairs (in iteration order). The layout
  // matches IceoryxTransportDriver::Send():
  //   pkg_size(4) + 1 + serialization_type.size() + 1 +
  //     sum_over_kv(2 + key.size() + 2 + val.size())
  static size_t ComputeCtxMetaHeaderSize(
      std::string_view serialization_type,
      const std::unordered_map<std::string_view, std::string_view>& kv) noexcept;

  // Encode the ctx-meta header into [buffer, buffer + buffer_size). Writes
  // the pkg_size field as `total_size`. Returns the number of bytes written
  // (i.e. ComputeCtxMetaHeaderSize for these inputs) on success, or 0 if
  // any precondition fails (oversize string, oversize KV count, buffer too
  // small).
  static size_t EncodeCtxMetaHeader(
      void* buffer, size_t buffer_size,
      uint32_t total_size,
      std::string_view serialization_type,
      const std::unordered_map<std::string_view, std::string_view>& kv) noexcept;

 private:
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  Options options_;
  std::atomic<State> state_ = State::kPreInit;

  const runtime::core::transport::TransportCatalog* transport_catalog_ptr_ = nullptr;

  IceoryxBus& iceoryx_bus_;

  std::unordered_map<
      std::string,
      std::shared_ptr<ibmw::runtime::core::transport::MessageHandlerTool>>
      message_handler_map_;

  // ----- Zero-copy POD path bookkeeping -----
  // Key: "<topic>|<msg_type>" (raw, not URL-encoded). Value: pointer borrowed
  // from iceoryx_bus_ (lifetime owned by the bus). Topics live until Shutdown.
  struct PodPublisherEndpoint {
    IoxPublisher* publisher;
    size_t initial_size_hint;
    size_t alignment;
  };
  std::unordered_map<std::string, PodPublisherEndpoint> pod_pub_map_;
};

// R8.2 mixed-API detection.
//
// These SFINAE traits inspect a user module type for two opt-in marker
// typedefs:
//   - `legacy_iox_marker`     -- typedef'd in modules that handwrite against
//                                the iceoryx_ext IoxStylePublisher<T> /
//                                IoxRawPodPublisher / IoxCtxMetaPublisher
//                                surface.
//   - `v2_driver_slot_marker` -- typedef'd in modules that go through the
//                                v2 publisher_for<T>() / subscriber_for<T>()
//                                factory in cpp/transport/transport_factory.h.
// A module that declares BOTH markers is mixing API surfaces (R8.2 violation)
// and instantiating IbmwR82SlotMixCheck<ModuleT> at namespace scope trips a
// compile-time static_assert. Pure compile-time -- no runtime cost.
template <typename ModuleT, typename = void>
struct IbmwUsesLegacyIceoryxApi : std::false_type {};

template <typename ModuleT>
struct IbmwUsesLegacyIceoryxApi<
    ModuleT, std::void_t<typename ModuleT::legacy_iox_marker>> : std::true_type {};

template <typename ModuleT, typename = void>
struct IbmwHasV2DriverSlot : std::false_type {};

template <typename ModuleT>
struct IbmwHasV2DriverSlot<
    ModuleT, std::void_t<typename ModuleT::v2_driver_slot_marker>> : std::true_type {};

template <typename ModuleT>
struct IbmwR82SlotMixCheck {
  static_assert(!(IbmwUsesLegacyIceoryxApi<ModuleT>::value &&
                  IbmwHasV2DriverSlot<ModuleT>::value),
                "R8.2: cannot mix v2 publisher_for<T>()/subscriber_for<T>() "
                "factory API with legacy iceoryx_ext IoxStylePublisher<T>/"
                "IoxRawPodPublisher/IoxCtxMetaPublisher in the same module. "
                "Pick one API surface per module. "
                "See skills/transport_decoupling.md §3.2.");
};

}  // namespace ibmw::extensions::iceoryx_extension
