// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// LoanedSubscriber<T> - typed receiver-side counterpart of LoanedSample<T>
// (Phase 7 §7.3 §U).
//
// Design
// ------
// LoanedSubscriber<T> is the user-level zero-copy receive fast lane.
// Like LoanedSample<T> on the publisher side, it talks to a thin
// transport reference (ReceiverRef) and the dispatcher routes the
// AttachLoaned() call to the first driver that supports zero-copy
// receive for the (topic, msg_type) pair. SHM-capable transport
// drivers override TransportDriver::AttachLoaned and feed the user's
// typed callback with a const-reference to the shared chunk payload
// directly — no MessageFrame, no deserialization, no filter chain,
// symmetric with the publisher Loan() fast lane.
//
// The wrapper is intentionally driver-agnostic: it talks only to
// ReceiverRef, whose vtable is filled by ReceiverAccess and ultimately
// routes to the dispatcher's AttachLoaned op (see
// transport_driver_dispatcher_ops.cc). A driver that cannot deliver
// zero-copy receive for the topic causes Attach() to return false. The API
// does not silently replace zero-copy delivery with OnMessage.
//
// Typical use
// -----------
//   auto subscriber = core_.GetTransport().GetSubscriber("hello_topic");
//   bool ok = ibmw::transport::LoanedSubscriber<MyPodMsg>::Attach(
//       subscriber, "hello_topic_msg",
//       [this](const MyPodMsg& msg) {
//         // direct read from the SHM chunk; pointer dies after this
//         // callback returns, do NOT retain.
//         process(msg);
//       });
//   MW_REQUIRE(ok, "zero-copy receive is unavailable");
//
// Constraints
// -----------
// * T must be trivially copyable (raw POD layout in the SHM chunk).
// * Callback runs on the SHM driver's listener thread; keep it short
//   and lock-free.
// * Callback MUST NOT retain the const T& past return.
// * Variable-size payloads (flexible-array-suffix layouts) are
//   supported via AttachBytes() which delivers (const T&, size_t bytes).

#pragma once

#include <cstddef>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "c/transport/transport_handle_base.h"
#include "c/transport/transport_context_base.h"
#include "cpp/transport/transport_handle.h"

namespace ibmw::transport {

template <typename T>
class LoanedSubscriber {
  static_assert(std::is_trivially_copyable_v<T>,
                "LoanedSubscriber<T> requires T to be trivially copyable.");

 public:
  // Fixed-size typed callback: receives a reference into the loaned
  // payload region. Pointer dies when the callback returns.
  using TypedCallback = std::function<void(const T& msg)>;

  // Variable-size typed callback: extra `size_t` is the loan-time
  // payload size (>= sizeof(T)) so users can iterate trailing bytes.
  using TypedCallbackBytes = std::function<void(const T& msg, size_t size)>;

  using RetainedCallback =
      std::function<void(const T& msg, size_t size, ReceivedLoan loan)>;

  // Attach a typed zero-copy receive callback to the named topic+type.
  //
  // Returns true if a driver claimed the loan path (typically a SHM
  // transport); false if no driver supports zero-copy receive for this
  // topic+type binding. The receiver remains attached for the lifetime
  // of the dispatcher (Init->Shutdown); the framework owns the
  // bookkeeping.
  static bool Attach(ReceiverRef receiver,
                     std::string_view msg_type,
                     TypedCallback cb) noexcept {
    try {
      if (!receiver) return false;
      auto cb_holder = std::move(cb);

      // C ABI loan callback signature:
      //   void(ctx_ptr, payload_ptr, payload_size, release_callback)
      // We bridge it into TypedCallback after a sizeof(T) sanity check.
      LoanedMessageCallback bridged(
        [cb_holder = std::move(cb_holder)](
            const ibmw_transport_context_base_t* /*ctx_ptr*/,
            const void* payload_ptr,
            size_t payload_size,
            ibmw_function_base_t* /*release_callback*/) {
          if (payload_ptr == nullptr || payload_size < sizeof(T)) return;
          try {
            cb_holder(*static_cast<const T*>(payload_ptr));
          } catch (...) {
            // Public callback boundary: user exceptions must not cross the
            // noexcept Function invoker used by the C ABI bridge.
          }
        });

      // msg_type_support is required by the dispatcher for symmetry with
      // OnMessage. Drivers that can do zero-copy receive (SHM transports)
      // ignore it (POD URL is keyed on (topic, msg_type) string only).
      // Pass the generic POD type-support so the call signature matches.
      return receiver.AttachLoaned(GetPodTypeSupport(msg_type),
                                   std::move(bridged));
    } catch (...) {
      return false;
    }
  }

  // Variable-size variant: callback is invoked with (T const&,
  // payload_size). Useful for flexible-array-suffix layouts where T
  // sits at offset 0 followed by trailing bytes.
  static bool AttachBytes(ReceiverRef receiver,
                          std::string_view msg_type,
                          TypedCallbackBytes cb) noexcept {
    try {
      if (!receiver) return false;
      auto cb_holder = std::move(cb);

      LoanedMessageCallback bridged(
        [cb_holder = std::move(cb_holder)](
            const ibmw_transport_context_base_t* /*ctx_ptr*/,
            const void* payload_ptr,
            size_t payload_size,
            ibmw_function_base_t* /*release_callback*/) {
          if (payload_ptr == nullptr || payload_size < sizeof(T)) return;
          try {
            cb_holder(*static_cast<const T*>(payload_ptr), payload_size);
          } catch (...) {
            // See Attach(): this bridge is a noexcept callback boundary.
          }
        });

      return receiver.AttachLoaned(GetPodTypeSupport(msg_type),
                                   std::move(bridged));
    } catch (...) {
      return false;
    }
  }

  static bool AttachLeaseBytes(ReceiverRef receiver,
                               std::string_view msg_type,
                               RetainedCallback cb,
                               LoanedSubscription* out_subscription,
                               RetainedLoanOptions options) noexcept {
    try {
      if (!receiver || !cb || out_subscription == nullptr ||
          options.max_outstanding == 0) {
        return false;
      }

      LoanedMessageCallback bridged(
        [cb_holder = std::move(cb)](
            const ibmw_transport_context_base_t* /*ctx_ptr*/,
            const void* payload_ptr,
            size_t payload_size,
            ibmw_function_base_t* release_callback) {
          ReceivedLoan loan(payload_ptr, payload_size, release_callback);
          if (payload_ptr == nullptr || payload_size < sizeof(T)) return;
          try {
            cb_holder(*static_cast<const T*>(payload_ptr), payload_size,
                      std::move(loan));
          } catch (...) {
            // Local/moved ReceivedLoan destruction releases exactly once.
          }
        });

      return receiver.AttachLoanedRetained(
          GetPodTypeSupport(msg_type), std::move(bridged), out_subscription,
          options);
    } catch (...) {
      return false;
    }
  }

 private:
  // The loan fast lane is keyed on (topic, msg_type) strings only —
  // SHM-capable drivers that override AttachLoaned do not deserialize
  // the payload, so they only need the type-name string for URL
  // composition, not a full type-support vtable. We hand them a thin
  // static stub that carries the type-name and trivially noop-reports
  // for size, so the dispatcher's TypeSupportRef::TypeName() lookup
  // succeeds.
  static const ibmw_type_support_base_t* GetPodTypeSupport(
      std::string_view msg_type) {
    // Per-call thread-local stub: msg_type lifetime is the caller's
    // responsibility (typically a string literal).
    thread_local ibmw_type_support_base_t stub{};
    thread_local std::string ts_storage;
    ts_storage.assign(msg_type);
    stub.type_name = [](void* ctx) -> ibmw_string_view_t {
      auto* s = static_cast<std::string*>(ctx);
      return ibmw_string_view_t{s->data(), s->size()};
    };
    stub.ctx = &ts_storage;
    return &stub;
  }
};

}  // namespace ibmw::transport
