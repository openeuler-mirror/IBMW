// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// IoxStylePublisher / IoxLoanedSample - typed user-facing wrappers around the
// iceoryx zero-copy POD path exposed by IceoryxTransportDriver.
//
// Mirrors the DDS extension's Ros2StylePublisher / LoanedMessage pattern so
// application code can switch transports with minimal source changes.
//
// Usage (publisher side):
//   auto pub = ibmw::extensions::iceoryx_extension::CreatePodPublisher<MyPod>(
//       driver, "topic", "MyPod");
//   auto sample = pub->borrow_loaned_sample();
//   if (sample.is_valid()) {
//     sample->field = 42;        // direct write into iceoryx SHM chunk
//     pub->publish(std::move(sample));
//   }
//
// Usage (subscriber side):
//   ibmw::extensions::iceoryx_extension::RegisterPodSubscriber<MyPod>(
//       driver, "topic", "MyPod",
//       [](const MyPod& msg) { /* read directly from SHM */ });

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "cpp/context/details/type_support.h"
#include "cpp/util/type_support.h"
#include "engine/transport/loaned_sample_handle.h"
#include "iceoryx_ext/iceoryx_transport_driver.h"

namespace ibmw::extensions::iceoryx_extension {

// RAII wrapper around a single LoanedSampleHandle for typed POD T.
// On destruction, if the handle was not consumed by publish(), the underlying
// chunk is released back to the iceoryx pool (no publish, no zombie loan).
template <typename T>
class IoxLoanedSample {
  static_assert(std::is_trivially_copyable_v<T>,
                "IoxLoanedSample<T> requires T to be trivially copyable.");

 public:
  IoxLoanedSample() = default;

  IoxLoanedSample(IceoryxTransportDriver* driver,
                  runtime::core::transport::LoanedSampleHandle handle)
      : driver_(driver), handle_(handle) {}

  ~IoxLoanedSample() { Release(); }

  IoxLoanedSample(const IoxLoanedSample&) = delete;
  IoxLoanedSample& operator=(const IoxLoanedSample&) = delete;

  IoxLoanedSample(IoxLoanedSample&& other) noexcept
      : driver_(other.driver_), handle_(other.handle_) {
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
    return driver_ != nullptr && handle_.user_payload_ptr != nullptr &&
           handle_.user_payload_size >= sizeof(T);
  }

  T* get() noexcept { return static_cast<T*>(handle_.user_payload_ptr); }
  const T* get() const noexcept { return static_cast<const T*>(handle_.user_payload_ptr); }

  T* operator->() noexcept { return get(); }
  const T* operator->() const noexcept { return get(); }

  T& operator*() noexcept { return *get(); }
  const T& operator*() const noexcept { return *get(); }

  // Internal: hand the handle to the publisher for transmission.
  // After this call the sample becomes invalid.
  runtime::core::transport::LoanedSampleHandle ExtractForPublish() noexcept {
    auto h = handle_;
    handle_ = {};
    driver_ = nullptr;
    return h;
  }

 private:
  void Release() noexcept {
    if (driver_ != nullptr && handle_.user_payload_ptr != nullptr) {
      driver_->ReleaseLoaned(handle_);
    }
    driver_ = nullptr;
    handle_ = {};
  }

  IceoryxTransportDriver* driver_ = nullptr;
  runtime::core::transport::LoanedSampleHandle handle_{};
};

// Typed POD publisher. Holds a TransportInfo built once at construction and
// dispatches to driver->LoanSample/PublishLoaned.
template <typename T>
class IoxStylePublisher {
  static_assert(std::is_trivially_copyable_v<T>,
                "IoxStylePublisher<T> requires T to be trivially copyable.");

 public:
  IoxStylePublisher(IceoryxTransportDriver* driver,
                    std::string topic_name,
                    std::string msg_type)
      : driver_(driver) {
    info_.topic_name = std::move(topic_name);
    info_.msg_type = std::move(msg_type);
  }

  IoxLoanedSample<T> borrow_loaned_sample() noexcept {
    runtime::core::transport::LoanedSampleHandle handle;
    bool ok = driver_->LoanSample(
        info_, sizeof(T), runtime::core::transport::LoanMode::kPodOnly,
        /*ctx=*/nullptr, handle);
    if (!ok) return {};
    return IoxLoanedSample<T>(driver_, handle);
  }

  // Variable-size variant. Loans `bytes` (must be >= sizeof(T)) so callers can
  // use a flexible array suffix (e.g. T at offset 0 followed by N bytes of
  // payload). The receive side is responsible for knowing the exact byte
  // length (via the chunk's user-payload size, which iceoryx supplies).
  IoxLoanedSample<T> borrow_loaned_sample(size_t bytes) noexcept {
    if (bytes < sizeof(T)) return {};
    runtime::core::transport::LoanedSampleHandle handle;
    bool ok = driver_->LoanSample(
        info_, bytes, runtime::core::transport::LoanMode::kPodOnly,
        /*ctx=*/nullptr, handle);
    if (!ok) return {};
    return IoxLoanedSample<T>(driver_, handle);
  }

  bool publish(IoxLoanedSample<T>&& sample) noexcept {
    if (!sample.is_valid()) return false;
    auto handle = sample.ExtractForPublish();
    driver_->PublishLoaned(handle);
    return true;
  }

  const std::string& topic_name() const noexcept { return info_.topic_name; }
  const std::string& msg_type() const noexcept { return info_.msg_type; }

 private:
  IceoryxTransportDriver* driver_;
  runtime::core::transport::TransportInfo info_;
};

// Factory: ensures the POD topic is registered on the driver, then returns a
// typed publisher. Returns nullptr on registration failure.
template <typename T>
std::shared_ptr<IoxStylePublisher<T>> CreatePodPublisher(
    IceoryxTransportDriver* driver,
    std::string_view topic,
    std::string_view msg_type) {
  static_assert(std::is_trivially_copyable_v<T>,
                "CreatePodPublisher<T> requires T to be trivially copyable.");
  if (driver == nullptr) return nullptr;
  if (!driver->EnsurePodTopic(topic, msg_type, sizeof(T))) return nullptr;
  return std::make_shared<IoxStylePublisher<T>>(
      driver, std::string(topic), std::string(msg_type));
}

// Subscriber registration helper. Callback takes a const T& read directly
// from the iceoryx SHM chunk; the chunk is released right after the callback
// returns, so the caller MUST NOT retain the reference.
template <typename T, typename Callback>
bool RegisterPodSubscriber(IceoryxTransportDriver* driver,
                           std::string_view topic,
                           std::string_view msg_type,
                           Callback&& cb) {
  static_assert(std::is_trivially_copyable_v<T>,
                "RegisterPodSubscriber<T> requires T to be trivially copyable.");
  if (driver == nullptr) return false;
  auto cb_holder = std::forward<Callback>(cb);
  return driver->RegisterPodSubscriber(
      topic, msg_type,
      [cb_holder = std::move(cb_holder)](const void* payload, size_t size) {
        if (size < sizeof(T)) return;
        cb_holder(*static_cast<const T*>(payload));
      });
}

// ============================================================================
// Raw (untyped) POD publisher for variable-size benchmarks.
// ============================================================================
//
// Same wire path as IoxStylePublisher<T> (URL prefix "/transport_zc_pod/...",
// no IBMW header), but the borrow API takes a runtime byte count instead of
// inferring from sizeof(T). Useful when the same binary needs to publish
// payloads of multiple sizes selected from configuration.
//
// The returned IoxLoanedSample<std::byte> exposes raw `data() / size()` via
// the existing typed wrapper -- we simply use std::byte as a stand-in for
// "untyped bytes". Callers cast the pointer to whatever struct they like.

class IoxRawPodPublisher {
 public:
  IoxRawPodPublisher(IceoryxTransportDriver* driver,
                     std::string topic_name,
                     std::string msg_type)
      : driver_(driver) {
    info_.topic_name = std::move(topic_name);
    info_.msg_type = std::move(msg_type);
  }

  // Borrow `bytes` of zero-copy SHM. Returns an empty handle on failure.
  IoxLoanedSample<std::byte> borrow_loaned_sample(size_t bytes) noexcept {
    if (bytes == 0) return {};
    runtime::core::transport::LoanedSampleHandle handle;
    bool ok = driver_->LoanSample(
        info_, bytes, runtime::core::transport::LoanMode::kPodOnly,
        /*ctx=*/nullptr, handle);
    if (!ok) return {};
    return IoxLoanedSample<std::byte>(driver_, handle);
  }

  bool publish(IoxLoanedSample<std::byte>&& sample) noexcept {
    if (!sample.is_valid()) return false;
    auto handle = sample.ExtractForPublish();
    driver_->PublishLoaned(handle);
    return true;
  }

  const std::string& topic_name() const noexcept { return info_.topic_name; }
  const std::string& msg_type() const noexcept { return info_.msg_type; }

 private:
  IceoryxTransportDriver* driver_;
  runtime::core::transport::TransportInfo info_;
};

// Factory mirroring CreatePodPublisher<T> but for the raw variant.
inline std::shared_ptr<IoxRawPodPublisher> CreateRawPodPublisher(
    IceoryxTransportDriver* driver,
    std::string_view topic,
    std::string_view msg_type,
    size_t initial_size_hint) {
  if (driver == nullptr) return nullptr;
  if (!driver->EnsurePodTopic(topic, msg_type, initial_size_hint))
    return nullptr;
  return std::make_shared<IoxRawPodPublisher>(
      driver, std::string(topic), std::string(msg_type));
}

// Raw subscriber registration: callback receives raw (ptr, size). The chunk
// is released right after the callback returns.
inline bool RegisterRawPodSubscriber(
    IceoryxTransportDriver* driver,
    std::string_view topic,
    std::string_view msg_type,
    std::function<void(const void* payload, size_t size)> cb) {
  if (driver == nullptr) return false;
  return driver->RegisterPodSubscriber(topic, msg_type, std::move(cb));
}

// ============================================================================
// ctx-meta zero-copy path
// ============================================================================
//
// Unlike the POD path, ctx-meta mode reuses the standard IBMW pub/sub stack
// on the receive side, so subscribers can use the regular framework
// transport/serialization layer transparently. The on-wire layout is
// identical to IceoryxTransportDriver::Send(), only the *send* path skips
// the buffer-array allocator and writes directly into the iceoryx SHM
// chunk -- end-to-end zero-copy.
//
// Usage:
//   ibmw::transport::Context ctx;
//   ctx.SetSerializationType("cdr");
//   ctx.SetMetaValue("trace_id", "abc123");
//
//   auto pub = ibmw::extensions::iceoryx_extension::CreateCtxMetaPublisher(
//       driver, "/img", "MyImage");           // RegisterSendType must
//                                             // already have been called.
//   auto loan = pub->borrow_loaned_buffer(ctx, /*payload_size=*/serialized.size());
//   if (loan.is_valid()) {
//     std::memcpy(loan.data(), serialized.data(), serialized.size());
//     pub->publish(std::move(loan));
//   }
//
// Note: borrow_loaned_buffer() returns a *byte buffer*; the caller controls
// the payload layout. This is intentional -- the framework's serializer
// already produces a contiguous byte stream (e.g. CDR/JSON), so handing
// users typed access here would force an extra layer of indirection without
// reducing copies. Callers that already have POD types should use the POD
// path (CreatePodPublisher) instead.

class IoxCtxMetaLoanedBuffer {
 public:
  IoxCtxMetaLoanedBuffer() = default;

  IoxCtxMetaLoanedBuffer(IceoryxTransportDriver* driver,
                         runtime::core::transport::LoanedSampleHandle handle)
      : driver_(driver), handle_(handle) {}

  ~IoxCtxMetaLoanedBuffer() { Release(); }

  IoxCtxMetaLoanedBuffer(const IoxCtxMetaLoanedBuffer&) = delete;
  IoxCtxMetaLoanedBuffer& operator=(const IoxCtxMetaLoanedBuffer&) = delete;

  IoxCtxMetaLoanedBuffer(IoxCtxMetaLoanedBuffer&& other) noexcept
      : driver_(other.driver_), handle_(other.handle_) {
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

  bool is_valid() const noexcept {
    return driver_ != nullptr && handle_.user_payload_ptr != nullptr;
  }

  void* data() noexcept { return handle_.user_payload_ptr; }
  const void* data() const noexcept { return handle_.user_payload_ptr; }
  size_t size() const noexcept { return handle_.user_payload_size; }

  // Internal: hand the handle to the publisher for transmission.
  // After this call the buffer becomes invalid.
  runtime::core::transport::LoanedSampleHandle ExtractForPublish() noexcept {
    auto h = handle_;
    handle_ = {};
    driver_ = nullptr;
    return h;
  }

 private:
  void Release() noexcept {
    if (driver_ != nullptr && handle_.user_payload_ptr != nullptr) {
      driver_->ReleaseLoaned(handle_);
    }
    driver_ = nullptr;
    handle_ = {};
  }

  IceoryxTransportDriver* driver_ = nullptr;
  runtime::core::transport::LoanedSampleHandle handle_{};
};

class IoxCtxMetaPublisher {
 public:
  IoxCtxMetaPublisher(IceoryxTransportDriver* driver,
                      std::string topic_name,
                      std::string msg_type)
      : driver_(driver) {
    info_.topic_name = std::move(topic_name);
    info_.msg_type = std::move(msg_type);
  }

  // Snapshot `ctx` and reserve `payload_size` bytes for the user payload.
  // The header (pkg_size + serialization_type + ctx-meta KVs) is filled at
  // borrow time; later edits to `ctx` will not be reflected.
  IoxCtxMetaLoanedBuffer borrow_loaned_buffer(
      const ibmw::transport::Context& ctx, size_t payload_size) noexcept {
    runtime::core::transport::LoanedSampleHandle handle;
    bool ok = driver_->LoanSample(
        info_, payload_size,
        runtime::core::transport::LoanMode::kCtxMetaThenPayload,
        &ctx, handle);
    if (!ok) return {};
    return IoxCtxMetaLoanedBuffer(driver_, handle);
  }

  bool publish(IoxCtxMetaLoanedBuffer&& buf) noexcept {
    if (!buf.is_valid()) return false;
    auto handle = buf.ExtractForPublish();
    driver_->PublishLoaned(handle);
    return true;
  }

  const std::string& topic_name() const noexcept { return info_.topic_name; }
  const std::string& msg_type() const noexcept { return info_.msg_type; }

 private:
  IceoryxTransportDriver* driver_;
  runtime::core::transport::TransportInfo info_;
};

// Factory: wraps an already-registered standard publisher. Caller MUST have
// invoked RegisterSendType() on the driver for (topic, msg_type) before
// borrowing any buffer. Returns nullptr if `driver` is null.
//
// Note: this factory does NOT call EnsurePodTopic (different URL prefix and
// different receive-side assumptions). The receive side is the regular IBMW
// MessageHandler flow.
inline std::shared_ptr<IoxCtxMetaPublisher> CreateCtxMetaPublisher(
    IceoryxTransportDriver* driver,
    std::string_view topic,
    std::string_view msg_type) {
  if (driver == nullptr) return nullptr;
  return std::make_shared<IoxCtxMetaPublisher>(
      driver, std::string(topic), std::string(msg_type));
}

// IoxCtxMetaTypedPublisher<T> -- one-call typed wrapper around
// IceoryxTransportDriver::LoanSerializedSampleWithCtx + PublishLoaned.
//
// Mirrors the standard SenderAccess<T>::Send(ctx, msg) UX but routes through
// the iceoryx zero-copy loan path: the message is CDR-serialized directly into
// an iceoryx SHM chunk, with the ctx-meta header (pkg_size + sertype + KV
// pairs) emitted in front of the payload. No intermediate heap copy.
//
// Usage:
//   auto pub = CreateCtxMetaTypedPublisher<MyMsg>(driver, "topic", "MyMsg");
//   ibmw::transport::Context ctx;
//   ctx.SetSerializationType("cdr");
//   pub->publish(ctx, msg);
//
// Pre-condition: caller has invoked
//   ibmw::transport::RegisterSendType<MyMsg>(senderRef)
// (or equivalent) on the standard SenderRef bound to (topic, MyMsg) so that
// the iceoryx publisher URL exists in the underlying iceoryx_bus. The typed
// publisher caches a TransportInfo populated with
// TypeSupportRef(GetMessageTypeSupport<MyMsg>()) at construction time and
// passes it back into the driver on every publish.
template <typename MsgType>
class IoxCtxMetaTypedPublisher {
 public:
  IoxCtxMetaTypedPublisher(IceoryxTransportDriver* driver,
                           std::string topic,
                           std::string msg_type)
      : driver_(driver),
        topic_(std::move(topic)),
        msg_type_(std::move(msg_type)) {
    info_.topic_name = topic_;
    info_.msg_type = msg_type_;
    info_.msg_type_support_ref =
        ibmw::util::TypeSupportRef(ibmw::GetMessageTypeSupport<MsgType>());
  }

  // Borrow an iceoryx chunk, serialize `msg` into it (growing the loan on
  // OOM), then publish. Returns false if driver is null or the loan/serialize
  // path failed; on failure, no chunk is leaked (driver releases on its own
  // error path, and PublishLoaned is not invoked).
  bool publish(const ibmw::transport::Context& ctx, const MsgType& msg) noexcept {
    if (driver_ == nullptr) return false;
    runtime::core::transport::LoanedSampleHandle handle;
    const bool ok = driver_->LoanSerializedSample(
        info_, ctx, static_cast<const void*>(&msg), handle);
    if (!ok) return false;
    driver_->PublishLoaned(handle);
    return true;
  }

  const std::string& topic_name() const noexcept { return info_.topic_name; }
  const std::string& msg_type() const noexcept { return info_.msg_type; }

 private:
  IceoryxTransportDriver* driver_;
  // TransportInfo stores string_view fields. Keep owned storage in the typed
  // wrapper so publish-time driver calls do not observe dangling views after
  // the constructor's by-value strings go out of scope.
  std::string topic_;
  std::string msg_type_;
  runtime::core::transport::TransportInfo info_;
};

// Factory: same pre-condition as CreateCtxMetaPublisher (caller must have
// registered the (topic, msg_type) publisher URL via RegisterSendType<T>()).
template <typename MsgType>
inline std::shared_ptr<IoxCtxMetaTypedPublisher<MsgType>>
CreateCtxMetaTypedPublisher(IceoryxTransportDriver* driver,
                            std::string_view topic,
                            std::string_view msg_type) {
  if (driver == nullptr) return nullptr;
  return std::make_shared<IoxCtxMetaTypedPublisher<MsgType>>(
      driver, std::string(topic), std::string(msg_type));
}

}  // namespace ibmw::extensions::iceoryx_extension
