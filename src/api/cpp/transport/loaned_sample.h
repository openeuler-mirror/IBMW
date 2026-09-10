// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// LoanedSample<T> - typed RAII wrapper around the IBMW user-level
// zero-copy loan fast lane (Phase 7 §7.3).
//
// Design
// ------
// LoanedSample<T> owns at most one outstanding LoanedSampleHandle obtained
// from SenderRef::Loan(). Lifecycle:
//
//   - Construction borrows nothing (default-constructed empty), or
//     adopts a handle returned by a publisher helper.
//   - Destructor releases an unconsumed handle back to the transport
//     via SenderRef::ReleaseLoaned(), preventing zombie loans.
//   - Move-only: a loan is unique.
//   - Publish path is two-step: ExtractForPublish() yields the raw
//     handle and clears the wrapper; the caller (typically a thin
//     publish() helper) hands the handle to SenderRef::PublishLoaned().
//
// The wrapper is intentionally driver-agnostic: it talks only to
// SenderRef, whose vtable is filled by SenderAccess and ultimately
// routes to the dispatcher's Loan / PublishLoaned / ReleaseLoaned ops
// (see transport_driver_dispatcher_ops.cc). PublishLoaned/ReleaseLoaned
// dispatch back to the issuing driver via handle.source_driver, so the
// wrapper never needs to know which transport supplied the chunk.
//
// Typical use
// -----------
//   auto sample = LoanedSample<Pose>::Borrow(sender_ref, "geometry/Pose");
//   if (sample.is_valid()) {
//     sample->x = 1.0;                          // direct write into SHM
//     sample->y = 2.0;
//     LoanedSample<Pose>::Publish(sender_ref, std::move(sample));
//   }
//
// Variable-size loans (e.g. flexible-array-suffix payloads) are
// supported via the (msg_type, bytes) overload of Borrow().
//
// Thread-safety: LoanedSample<T> is not thread-safe; transfer ownership
// across threads via std::move only.

#pragma once

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include "cpp/transport/transport_handle.h"

namespace ibmw::transport {

template <typename T>
class LoanedSample {
  static_assert(std::is_trivially_copyable_v<T>,
                "LoanedSample<T> requires T to be trivially copyable.");

 public:
  /// Empty / consumed state.
  LoanedSample() = default;

  /// Adopt an already-borrowed handle. The wrapper records the
  /// SenderRef so a later destructor can release the handle if needed.
  /// Prefer the static Borrow()/Publish() helpers below over manual
  /// construction.
  LoanedSample(SenderRef sender, LoanedSampleHandle handle) noexcept
      : sender_(sender), handle_(handle) {}

  ~LoanedSample() { Release(); }

  LoanedSample(const LoanedSample&) = delete;
  LoanedSample& operator=(const LoanedSample&) = delete;

  LoanedSample(LoanedSample&& other) noexcept
      : sender_(other.sender_), handle_(other.handle_) {
    other.sender_ = SenderRef();
    other.handle_ = LoanedSampleHandle{};
  }

  LoanedSample& operator=(LoanedSample&& other) noexcept {
    if (this != &other) {
      Release();
      sender_ = other.sender_;
      handle_ = other.handle_;
      other.sender_ = SenderRef();
      other.handle_ = LoanedSampleHandle{};
    }
    return *this;
  }

  /// True iff the wrapper holds a usable loan: payload pointer is set
  /// and the loaned region is large enough for T. Variable-size loans
  /// always satisfy size >= sizeof(T) because Borrow(bytes) rejects
  /// undersized requests.
  bool is_valid() const noexcept {
    return handle_.user_payload_ptr != nullptr &&
           handle_.user_payload_size >= sizeof(T);
  }

  T* get() noexcept { return static_cast<T*>(handle_.user_payload_ptr); }
  const T* get() const noexcept {
    return static_cast<const T*>(handle_.user_payload_ptr);
  }

  T* operator->() noexcept { return get(); }
  const T* operator->() const noexcept { return get(); }

  T& operator*() noexcept { return *get(); }
  const T& operator*() const noexcept { return *get(); }

  /// Size in bytes of the loaned payload region. For fixed-size loans
  /// this equals sizeof(T) (or whatever the driver rounded up to); for
  /// variable-size loans it is the value passed to Borrow(bytes).
  size_t size_bytes() const noexcept { return handle_.user_payload_size; }

  /// Release the underlying chunk back to the transport without
  /// publishing. Idempotent; subsequent calls are no-ops. The
  /// destructor calls this for you on unconsumed loans.
  void Release() noexcept {
    if (handle_.source_driver != nullptr && sender_) {
      sender_.ReleaseLoaned(handle_);
    }
    sender_ = SenderRef();
    handle_ = LoanedSampleHandle{};
  }

  /// Hand the raw handle to the caller and clear the wrapper. The
  /// caller takes responsibility for invoking SenderRef::PublishLoaned
  /// (or ReleaseLoaned) on the returned handle. Used by Publish() and
  /// by users who need finer-grained control.
  LoanedSampleHandle ExtractForPublish() noexcept {
    auto h = handle_;
    handle_ = LoanedSampleHandle{};
    sender_ = SenderRef();
    return h;
  }

  // -- static convenience helpers -----------------------------------

  /// Borrow a fixed-size loan (sizeof(T) bytes) from the named topic
  /// type on the given publisher. Returns an empty wrapper on failure
  /// (no driver supports the loan, dispatcher not Started, etc.).
  /// Wire-format mode defaults to kPodOnly (raw payload).
  static LoanedSample Borrow(SenderRef sender,
                             std::string_view msg_type,
                             LoanMode mode = kLoanModePodOnly) noexcept {
    if (!sender) return {};
    auto handle = sender.Loan(msg_type, sizeof(T), mode);
    if (handle.user_payload_ptr == nullptr) return {};
    return LoanedSample(sender, handle);
  }

  /// Variable-size variant. `bytes` MUST be >= sizeof(T); requests
  /// smaller than sizeof(T) return an empty wrapper without contacting
  /// the transport. Useful for flexible-array-suffix payload layouts
  /// where T sits at offset 0 followed by trailing bytes.
  ///
  /// Named distinctly from Borrow() to avoid overload ambiguity when
  /// callers pass an integer literal that LoanMode (uint8_t) could
  /// also accept.
  static LoanedSample BorrowBytes(SenderRef sender,
                                  std::string_view msg_type,
                                  size_t bytes,
                                  LoanMode mode = kLoanModePodOnly) noexcept {
    if (!sender || bytes < sizeof(T)) return {};
    auto handle = sender.Loan(msg_type, bytes, mode);
    if (handle.user_payload_ptr == nullptr) return {};
    return LoanedSample(sender, handle);
  }

  /// Publish a previously-borrowed sample. Returns false if the sample
  /// is empty / already consumed; in that case the loan, if any, is
  /// released by the destructor of the rvalue argument.
  static bool Publish(SenderRef sender, LoanedSample&& sample) noexcept {
    if (!sample.is_valid() || !sender) return false;
    auto handle = sample.ExtractForPublish();
    sender.PublishLoaned(handle);
    return true;
  }

 private:
  SenderRef sender_;
  LoanedSampleHandle handle_{};
};

}  // namespace ibmw::transport
