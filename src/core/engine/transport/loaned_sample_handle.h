// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// LoanedSampleHandle - core abstraction for end-to-end zero-copy publishing.
//
// Background
// ----------
// Some transports (notably iceoryx and DDS with Plain types) can hand the
// publisher application a pre-allocated buffer that lives in the destination
// shared-memory region. Writing the message directly into that buffer and
// then "publishing" the buffer descriptor avoids a memcpy on the send side
// and (for SHM transports) on the receive side as well.
//
// This header defines a *transport-agnostic* handle so that:
//   - applications get a uniform borrow/publish/release surface;
//   - transports that don't support loaned samples return false from
//     LoanSample(...); this API does not silently publish through Send();
//   - the core abstraction does NOT pull in any transport-specific headers.
//
// Wire-format modes
// -----------------
// LoanedSampleHandle carries a `mode` enum. Two modes are public wire-format
// requests:
//   - kPodOnly      : the loaned region holds only the user payload bytes.
//                     The receiver MUST reinterpret_cast to the same trivially-
//                     copyable T. ctx-meta and the IBMW serialization framework
//                     are NOT used on this path.
//   - kCtxMetaThenPayload :
//                     the loaned region begins with a fixed-size ctx-meta
//                     header (driver-defined, opaque to the application) and
//                     is followed by the user payload bytes. user_payload_ptr
//                     points to the start of the payload region; the header
//                     is populated by the driver when PublishLoaned is called.
//
// Threading
// ---------
// A LoanedSampleHandle is a unique resource. The owner must call exactly one
// of PublishLoaned() / ReleaseLoaned() before destroying it. Drivers track
// outstanding loans internally (typically via a token).

#pragma once

#include <cstddef>
#include <cstdint>

namespace ibmw::runtime::core::transport {

class TransportDriver;  // forward decl; included in transport_driver.h

enum class LoanMode : uint8_t {
  kPodOnly = 0,                ///< raw payload, no header
  kCtxMetaThenPayload = 1,     ///< driver-private header followed by payload
  // Internal completion discriminator used by transports whose reported and
  // unreported ctx-meta loans have different driver-token layouts. Callers
  // must continue to request kCtxMetaThenPayload.
  kCtxMetaThenPayloadUnreported = 2,
};

/**
 * @brief Opaque handle returned by TransportDriver::LoanSample().
 *
 * The handle is filled in by the driver and consumed by the application:
 *   - Application writes into [user_payload_ptr, user_payload_ptr + user_payload_size).
 *   - Application invokes TransportDriver::PublishLoaned(handle) to publish, OR
 *     TransportDriver::ReleaseLoaned(handle) to discard without publishing.
 *
 * `driver_token` is owned by the driver and uninterpreted by the core; it
 * typically points to the driver-side bookkeeping for the loan (e.g. an
 * iox::popo::UntypedPublisher chunk pointer).
 *
 * `source_driver` records which driver issued the loan, so that
 * dispatcher- and user-level wrappers (LoanedSample<T>) can route
 * Publish/Release back to the correct driver without an extra lookup.
 * Set by TransportDriver::LoanSample() implementations on success.
 *
 * Phase 7 §7.3 §K-3.3: added source_driver field to support
 * dispatcher-level Loan/PublishLoaned ops that fan out across drivers.
 */
struct LoanedSampleHandle {
  /// Pointer to the start of the user-writable payload region.
  /// Valid until PublishLoaned() or ReleaseLoaned() is called.
  void* user_payload_ptr = nullptr;

  /// Size in bytes of the user-writable payload region.
  size_t user_payload_size = 0;

  /// Layout mode of the loaned region.
  LoanMode mode = LoanMode::kPodOnly;

  /// Driver-private bookkeeping. The core never dereferences this pointer.
  void* driver_token = nullptr;

  /// Driver that issued this loan. Used by dispatcher/wrapper layers to
  /// route Publish/Release calls without a topic-to-driver lookup.
  /// Drivers MUST set this in LoanSample() on success.
  TransportDriver* source_driver = nullptr;
};

}  // namespace ibmw::runtime::core::transport
