// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Source-compatible counterpart of iceoryx_ext/iox_style_publisher.h on the
// shm driver: same class, function and method names, signatures and
// semantics, in ibmw::extensions::shm_extension. Porting iceoryx_ext code
// means changing the include ("iceoryx_ext/iox_style_publisher.h" ->
// "shm_ext/iox_style_publisher.h") and the namespace (iceoryx_extension ->
// shm_extension); IceoryxTransportDriver names ShmTransportDriver here, and
// the R8.2 mixed-API check (IbmwR82SlotMixCheck) is provided as well.
//
// One deliberate difference: publish() returns what really happened. It is
// false when the sample/buffer is invalid or when the driver did not publish
// the chunk (stale or duplicate loan, foreign handle); iceoryx_ext returns
// true for every valid sample.
//
// Wire formats are the shm driver's: the POD path is kPodOnly under
// "/transport_zc_pod/<topic>/<msg_type>" (receive side RegisterPodSubscriber),
// the ctx-meta path is the frame Send() produces (receive side OnMessage<T>).
//
// Pre-conditions, as for iceoryx_ext: CreatePodPublisher / CreateRawPodPublisher
// run while the driver is in Init state (after Engine::Initialize, before
// Engine::Start); the ctx-meta publishers need RegisterSendType for
// (topic, msg_type) first. Every class keeps a raw ShmTransportDriver*, so it
// must not outlive the driver; publish or drop every loan before engine
// shutdown (a loan still held makes the shutdown preparation time out).
//
//   namespace shm_ext = ibmw::extensions::shm_extension;
//   auto pub = shm_ext::CreatePodPublisher<MyPod>(driver, "topic", "MyPod");
//   auto sample = pub->borrow_loaned_sample();
//   if (sample.is_valid()) {
//     sample->field = 42;  // written in place in the shm chunk
//     pub->publish(std::move(sample));
//   }
//   shm_ext::RegisterPodSubscriber<MyPod>(driver, "topic", "MyPod",
//                                         [](const MyPod& msg) { /* ... */ });

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "cpp/context/details/type_support.h"
#include "cpp/transport/transport_context.h"
#include "cpp/util/type_support.h"
#include "engine/transport/loaned_sample_handle.h"
#include "shm_ext/shm_transport_driver.h"

namespace ibmw::extensions::shm_extension {

// Name of the driver type in iceoryx_ext signatures. Only a C++ alias: the
// driver slot stays ShmTransportDriver's, so IBMW_USE_DRIVER binds this type
// and never the iceoryx driver.
using IceoryxTransportDriver = ShmTransportDriver;

// Move-only loan of a typed POD T. Destroying it unpublished returns the
// chunk to the driver.
template <typename T>
class IoxLoanedSample {
  static_assert(std::is_trivially_copyable_v<T>, "IoxLoanedSample<T> requires T to be trivially copyable.");

 public:
  IoxLoanedSample() = default;
  IoxLoanedSample(ShmTransportDriver* driver, runtime::core::transport::LoanedSampleHandle handle)
      : driver_(driver), handle_(handle) {}
  ~IoxLoanedSample() { Release(); }

  IoxLoanedSample(const IoxLoanedSample&) = delete;
  IoxLoanedSample& operator=(const IoxLoanedSample&) = delete;
  IoxLoanedSample(IoxLoanedSample&& other) noexcept : driver_(other.driver_), handle_(other.handle_) {
    other.driver_ = nullptr;
    other.handle_ = {};
  }
  IoxLoanedSample& operator=(IoxLoanedSample&& other) noexcept {
    if (this != &other) {
      Release();
      driver_ = other.driver_;
      handle_ = other.handle_;
      other.driver_ = nullptr;
      other.handle_ = {};
    }
    return *this;
  }

  bool is_valid() const noexcept {
    return driver_ != nullptr && handle_.user_payload_ptr != nullptr && handle_.user_payload_size >= sizeof(T);
  }

  T* get() noexcept { return static_cast<T*>(handle_.user_payload_ptr); }
  const T* get() const noexcept { return static_cast<const T*>(handle_.user_payload_ptr); }
  T* operator->() noexcept { return get(); }
  const T* operator->() const noexcept { return get(); }
  T& operator*() noexcept { return *get(); }
  const T& operator*() const noexcept { return *get(); }

  // Hands the handle over for publishing; the sample becomes invalid.
  runtime::core::transport::LoanedSampleHandle ExtractForPublish() noexcept {
    auto h = handle_;
    handle_ = {};
    driver_ = nullptr;
    return h;
  }

 private:
  void Release() noexcept {
    if (driver_ != nullptr && handle_.user_payload_ptr != nullptr) driver_->ReleaseLoaned(handle_);
    driver_ = nullptr;
    handle_ = {};
  }

  ShmTransportDriver* driver_ = nullptr;
  runtime::core::transport::LoanedSampleHandle handle_{};
};

// Typed POD publisher; the TransportInfo is built once at construction.
template <typename T>
class IoxStylePublisher {
  static_assert(std::is_trivially_copyable_v<T>, "IoxStylePublisher<T> requires T to be trivially copyable.");

 public:
  IoxStylePublisher(ShmTransportDriver* driver, std::string topic_name, std::string msg_type) : driver_(driver) {
    info_.topic_name = std::move(topic_name);
    info_.msg_type = std::move(msg_type);
  }

  IoxLoanedSample<T> borrow_loaned_sample() noexcept { return Borrow(sizeof(T)); }

  // Loans `bytes` (at least sizeof(T)) for a T followed by a variable-size
  // tail; the subscriber gets the loaned size with the payload.
  IoxLoanedSample<T> borrow_loaned_sample(size_t bytes) noexcept {
    if (bytes < sizeof(T)) return {};
    return Borrow(bytes);
  }

  // False if the sample is invalid or its chunk was not published.
  bool publish(IoxLoanedSample<T>&& sample) noexcept {
    if (!sample.is_valid()) return false;
    auto handle = sample.ExtractForPublish();
    return driver_->TryPublishLoaned(handle);
  }

  const std::string& topic_name() const noexcept { return info_.topic_name; }
  const std::string& msg_type() const noexcept { return info_.msg_type; }

 private:
  IoxLoanedSample<T> Borrow(size_t bytes) noexcept {
    runtime::core::transport::LoanedSampleHandle handle;
    if (!driver_->LoanSample(info_, bytes, runtime::core::transport::LoanMode::kPodOnly, /*ctx=*/nullptr, handle)) {
      return {};
    }
    return IoxLoanedSample<T>(driver_, handle);
  }

  ShmTransportDriver* driver_;
  runtime::core::transport::TransportInfo info_;
};

// Registers the POD topic (size hint sizeof(T)); nullptr if `driver` is null
// or registration fails (wrong state, conflicting size hint).
template <typename T>
std::shared_ptr<IoxStylePublisher<T>> CreatePodPublisher(ShmTransportDriver* driver, std::string_view topic,
                                                         std::string_view msg_type) {
  static_assert(std::is_trivially_copyable_v<T>, "CreatePodPublisher<T> requires T to be trivially copyable.");
  if (driver == nullptr) return nullptr;
  if (!driver->EnsurePodTopic(topic, msg_type, sizeof(T))) return nullptr;
  return std::make_shared<IoxStylePublisher<T>>(driver, std::string(topic), std::string(msg_type));
}

// `cb` gets a const T& that points into the shm chunk, which is released when
// it returns: do not keep the reference. Payloads smaller than T are skipped.
template <typename T, typename Callback>
bool RegisterPodSubscriber(ShmTransportDriver* driver, std::string_view topic, std::string_view msg_type,
                           Callback&& cb) {
  static_assert(std::is_trivially_copyable_v<T>, "RegisterPodSubscriber<T> requires T to be trivially copyable.");
  if (driver == nullptr) return false;
  auto cb_holder = std::forward<Callback>(cb);
  return driver->RegisterPodSubscriber(topic, msg_type,
                                       [cb_holder = std::move(cb_holder)](const void* payload, size_t size) {
                                         if (size < sizeof(T)) return;
                                         cb_holder(*static_cast<const T*>(payload));
                                       });
}

// Untyped POD publisher on the same wire path: the byte count is chosen per
// loan, std::byte stands for "untyped".
class IoxRawPodPublisher {
 public:
  IoxRawPodPublisher(ShmTransportDriver* driver, std::string topic_name, std::string msg_type) : driver_(driver) {
    info_.topic_name = std::move(topic_name);
    info_.msg_type = std::move(msg_type);
  }

  // Invalid sample if `bytes` is 0 or the loan fails.
  IoxLoanedSample<std::byte> borrow_loaned_sample(size_t bytes) noexcept {
    if (bytes == 0) return {};
    runtime::core::transport::LoanedSampleHandle handle;
    if (!driver_->LoanSample(info_, bytes, runtime::core::transport::LoanMode::kPodOnly, /*ctx=*/nullptr, handle)) {
      return {};
    }
    return IoxLoanedSample<std::byte>(driver_, handle);
  }

  // False if the sample is invalid or its chunk was not published.
  bool publish(IoxLoanedSample<std::byte>&& sample) noexcept {
    if (!sample.is_valid()) return false;
    auto handle = sample.ExtractForPublish();
    return driver_->TryPublishLoaned(handle);
  }

  const std::string& topic_name() const noexcept { return info_.topic_name; }
  const std::string& msg_type() const noexcept { return info_.msg_type; }

 private:
  ShmTransportDriver* driver_;
  runtime::core::transport::TransportInfo info_;
};

// Registers the POD topic; nullptr if `driver` is null or registration fails.
inline std::shared_ptr<IoxRawPodPublisher> CreateRawPodPublisher(ShmTransportDriver* driver, std::string_view topic,
                                                                 std::string_view msg_type,
                                                                 size_t initial_size_hint) {
  if (driver == nullptr) return nullptr;
  if (!driver->EnsurePodTopic(topic, msg_type, initial_size_hint)) return nullptr;
  return std::make_shared<IoxRawPodPublisher>(driver, std::string(topic), std::string(msg_type));
}

// `cb` gets (ptr, size) into the shm chunk, released when it returns.
inline bool RegisterRawPodSubscriber(ShmTransportDriver* driver, std::string_view topic, std::string_view msg_type,
                                     std::function<void(const void* payload, size_t size)> cb) {
  if (driver == nullptr) return false;
  return driver->RegisterPodSubscriber(topic, msg_type, std::move(cb));
}

// Move-only ctx-meta loan: the ctx-meta header is already written in front of
// data(), which holds size() bytes for the caller's serialized payload.
// Destroying it unpublished returns the chunk to the driver.
class IoxCtxMetaLoanedBuffer {
 public:
  IoxCtxMetaLoanedBuffer() = default;
  IoxCtxMetaLoanedBuffer(ShmTransportDriver* driver, runtime::core::transport::LoanedSampleHandle handle)
      : driver_(driver), handle_(handle) {}
  ~IoxCtxMetaLoanedBuffer() { Release(); }

  IoxCtxMetaLoanedBuffer(const IoxCtxMetaLoanedBuffer&) = delete;
  IoxCtxMetaLoanedBuffer& operator=(const IoxCtxMetaLoanedBuffer&) = delete;
  IoxCtxMetaLoanedBuffer(IoxCtxMetaLoanedBuffer&& other) noexcept : driver_(other.driver_), handle_(other.handle_) {
    other.driver_ = nullptr;
    other.handle_ = {};
  }
  IoxCtxMetaLoanedBuffer& operator=(IoxCtxMetaLoanedBuffer&& other) noexcept {
    if (this != &other) {
      Release();
      driver_ = other.driver_;
      handle_ = other.handle_;
      other.driver_ = nullptr;
      other.handle_ = {};
    }
    return *this;
  }

  bool is_valid() const noexcept { return driver_ != nullptr && handle_.user_payload_ptr != nullptr; }

  void* data() noexcept { return handle_.user_payload_ptr; }
  const void* data() const noexcept { return handle_.user_payload_ptr; }
  size_t size() const noexcept { return handle_.user_payload_size; }

  // Hands the handle over for publishing; the buffer becomes invalid.
  runtime::core::transport::LoanedSampleHandle ExtractForPublish() noexcept {
    auto h = handle_;
    handle_ = {};
    driver_ = nullptr;
    return h;
  }

 private:
  void Release() noexcept {
    if (driver_ != nullptr && handle_.user_payload_ptr != nullptr) driver_->ReleaseLoaned(handle_);
    driver_ = nullptr;
    handle_ = {};
  }

  ShmTransportDriver* driver_ = nullptr;
  runtime::core::transport::LoanedSampleHandle handle_{};
};

// Untyped ctx-meta publisher of a standard topic: the receive side is the
// regular OnMessage stack, the caller writes the serialized payload itself.
class IoxCtxMetaPublisher {
 public:
  IoxCtxMetaPublisher(ShmTransportDriver* driver, std::string topic_name, std::string msg_type) : driver_(driver) {
    info_.topic_name = std::move(topic_name);
    info_.msg_type = std::move(msg_type);
  }

  // Writes the header from `ctx` (serialization type + meta) now, so later
  // edits of `ctx` do not apply, and reserves `payload_size` bytes behind it.
  // Invalid buffer if the topic has no RegisterSendType or the loan fails.
  IoxCtxMetaLoanedBuffer borrow_loaned_buffer(const ibmw::transport::Context& ctx, size_t payload_size) noexcept {
    runtime::core::transport::LoanedSampleHandle handle;
    if (!driver_->LoanSample(info_, payload_size, runtime::core::transport::LoanMode::kCtxMetaThenPayload, &ctx,
                             handle)) {
      return {};
    }
    return IoxCtxMetaLoanedBuffer(driver_, handle);
  }

  // False if the buffer is invalid or its chunk was not published.
  bool publish(IoxCtxMetaLoanedBuffer&& buf) noexcept {
    if (!buf.is_valid()) return false;
    auto handle = buf.ExtractForPublish();
    return driver_->TryPublishLoaned(handle);
  }

  const std::string& topic_name() const noexcept { return info_.topic_name; }
  const std::string& msg_type() const noexcept { return info_.msg_type; }

 private:
  ShmTransportDriver* driver_;
  runtime::core::transport::TransportInfo info_;
};

// nullptr if `driver` is null. Does not register anything: RegisterSendType
// for (topic, msg_type) must have happened before the first borrow.
inline std::shared_ptr<IoxCtxMetaPublisher> CreateCtxMetaPublisher(ShmTransportDriver* driver, std::string_view topic,
                                                                   std::string_view msg_type) {
  if (driver == nullptr) return nullptr;
  return std::make_shared<IoxCtxMetaPublisher>(driver, std::string(topic), std::string(msg_type));
}

// Typed ctx-meta publisher: publish(ctx, msg) serializes `msg` straight into
// a loaned chunk behind the ctx-meta header (LoanSerializedSample), then
// publishes it. Same pre-condition as CreateCtxMetaPublisher.
template <typename MsgType>
class IoxCtxMetaTypedPublisher {
 public:
  IoxCtxMetaTypedPublisher(ShmTransportDriver* driver, std::string topic, std::string msg_type) : driver_(driver) {
    info_.topic_name = std::move(topic);
    info_.msg_type = std::move(msg_type);
    info_.msg_type_support_ref = ibmw::util::TypeSupportRef(ibmw::GetMessageTypeSupport<MsgType>());
  }

  // False if `driver` is null, the loan or the serialization failed (the
  // driver then holds no chunk) or the chunk was not published.
  bool publish(const ibmw::transport::Context& ctx, const MsgType& msg) noexcept {
    if (driver_ == nullptr) return false;
    runtime::core::transport::LoanedSampleHandle handle;
    if (!driver_->LoanSerializedSample(info_, ctx, static_cast<const void*>(&msg), handle)) return false;
    return driver_->TryPublishLoaned(handle);
  }

  const std::string& topic_name() const noexcept { return info_.topic_name; }
  const std::string& msg_type() const noexcept { return info_.msg_type; }

 private:
  ShmTransportDriver* driver_;
  runtime::core::transport::TransportInfo info_;
};

// nullptr if `driver` is null; same pre-condition as CreateCtxMetaPublisher.
template <typename MsgType>
inline std::shared_ptr<IoxCtxMetaTypedPublisher<MsgType>> CreateCtxMetaTypedPublisher(ShmTransportDriver* driver,
                                                                                       std::string_view topic,
                                                                                       std::string_view msg_type) {
  if (driver == nullptr) return nullptr;
  return std::make_shared<IoxCtxMetaTypedPublisher<MsgType>>(driver, std::string(topic), std::string(msg_type));
}

// R8.2 trip wire, identical to iceoryx_ext's: a module typedef'ing both
// `legacy_iox_marker` (handwritten against the helpers above) and
// `v2_driver_slot_marker` (publisher_for<T>() / subscriber_for<T>()) mixes API
// surfaces, and instantiating IbmwR82SlotMixCheck<ModuleT> fails to compile.
template <typename ModuleT, typename = void>
struct IbmwUsesLegacyIceoryxApi : std::false_type {};

template <typename ModuleT>
struct IbmwUsesLegacyIceoryxApi<ModuleT, std::void_t<typename ModuleT::legacy_iox_marker>> : std::true_type {};

template <typename ModuleT, typename = void>
struct IbmwHasV2DriverSlot : std::false_type {};

template <typename ModuleT>
struct IbmwHasV2DriverSlot<ModuleT, std::void_t<typename ModuleT::v2_driver_slot_marker>> : std::true_type {};

template <typename ModuleT>
struct IbmwR82SlotMixCheck {
  static_assert(!(IbmwUsesLegacyIceoryxApi<ModuleT>::value && IbmwHasV2DriverSlot<ModuleT>::value),
                "R8.2: cannot mix v2 publisher_for<T>()/subscriber_for<T>() "
                "factory API with legacy IoxStylePublisher<T>/"
                "IoxRawPodPublisher/IoxCtxMetaPublisher in the same module. "
                "Pick one API surface per module. "
                "See skills/transport_decoupling.md §3.2.");
};

}  // namespace ibmw::extensions::shm_extension
