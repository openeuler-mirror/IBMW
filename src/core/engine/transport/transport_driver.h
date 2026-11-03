// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/driver_slot.h"
#include "engine/transport/loaned_sample_handle.h"
#include "engine/transport/transport_catalog.h"
#include "engine/transport/transport_export.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::transport {
class Context;  // fwd decl: cpp/transport/transport_context.h
}  // namespace ibmw::transport

namespace ibmw::runtime::core::transport {

class IBMW_TRANSPORT_EXPORT TransportDriver {
 public:
  TransportDriver() = default;
  virtual ~TransportDriver() = default;

  TransportDriver(const TransportDriver&) = delete;
  TransportDriver& operator=(const TransportDriver&) = delete;

  virtual std::string_view Name() const noexcept = 0;  // It should always return the same value

  /**
   * @brief Stable cross-.so driver slot id (typed-slot pkg ABI).
   *
   * Returns the FNV-1a 64-bit hash of the driver's stable string id
   * (e.g. "ibmw.transport.dds"). User pkgs reference this slot id at
   * compile time via IBMW_DRIVER_ID(DriverT) and dispatch incoming
   * driver pointers in Module::SetDriverSlot(slot, ptr) without ever
   * touching a transport-name C-string. Decouples user pkgs from
   * transport-specific naming (skill R7 G6).
   *
   * Subclasses must implement via the IBMW_DECLARE_DRIVER_SLOT(...)
   * macro to keep kStableDriverId and Slot() in lockstep.
   */
  virtual ibmw_driver_slot_t Slot() const noexcept = 0;

  // Implementations assign their effective options back to options_node.
  // YAML::Node shares storage with the caller's tree, so this fills in the
  // config that --dump_cfg writes out; it is not a dead store.
  virtual void Initialize(YAML::Node options_node) = 0;
  virtual void Start() = 0;
  virtual void Shutdown() = 0;

  virtual std::list<std::pair<std::string, std::string>> GenInitializationReport() const noexcept { return {}; }

  /**
   * @brief Set the Transport Catalog to driver
   * @note
   * 1. This method will only be called once before 'Initialize'.
   *
   * @param transport_catalog_ptr
   */
  virtual void SetTransportCatalog(const TransportCatalog* transport_catalog_ptr) noexcept {}

  /**
   * @brief Register publish type
   * @note
   * 1. This method will only be called after 'Initialize' and before 'Start'.
   *
   * @param send_type_wrapper
   * @return Register result
   */
  virtual bool RegisterSendType(
      const SendTypeWrapper& send_type_wrapper) noexcept = 0;

  /**
   * @brief Subscribe
   * @note
   * 1. This method will only be called after 'Initialize' and before 'Start'.
   *
   * @param message_handler
   * @return Subscribe result
   */
  virtual bool OnMessage(const MessageHandlerWrapper& message_handler) noexcept = 0;

  /**
   * @brief Subscribe via the zero-copy receive fast lane.
   *
   * Phase 7 §7.3 §U: receiver-side counterpart of LoanSample. Drivers
   * that can deliver the underlying buffer pointer to the user without
   * deserialization (typically SHM transports such as iceoryx) override
   * this. The dispatcher invokes drivers in registration order
   * first-success-wins.
   *
   * Default implementation returns false (zero-copy receive not
   * supported). The dispatcher surfaces false to the caller; this API does
   * not silently replace zero-copy delivery with OnMessage.
   *
   * The callback embedded in `message_handler.callback` is a
   * `LoanedMessageCallback` (see transport_catalog.h) — driver invokes
   * it directly with (ctx_ptr, payload_ptr, payload_size, release).
   *
   * @param message_handler
   * @return Subscribe result (false if driver cannot do zero-copy
   *         receive for this topic+type)
   */
  virtual bool AttachLoaned(const LoanedMessageHandlerWrapper& /*message_handler*/) noexcept {
    return false;
  }

  /**
   * @brief Publish
   * @note
   * 1. This method will only be called after 'Start' and before 'Shutdown'.
   *
   * @param publish_wrapper
   */
  virtual void Send(MessageFrame& msg_wrapper) noexcept = 0;

  /**
   * @brief Borrow a loaned sample from the transport's destination buffer.
   *
   * Default implementation returns false (loan not supported). SHM-capable
   * transports such as iceoryx may override this to return a pointer into
   * their shared-memory chunk so the application can fill the payload in-place
   * and avoid a serialization/copy step.
   *
   * Phase 7 §X1: ctx parameter unifies the previously iceoryx-only
   * LoanSampleWithCtx() extension into the virtual function family.
   *
   * @param info             topic/type identification (must match a previously
   *                         registered SendType).
   * @param requested_size   user payload size in bytes.
   * @param mode             desired wire-format layout (see LoanMode).
   * @param ctx              ctx-meta source for kCtxMetaThenPayload mode;
   *                         must be non-null in that mode and ignored in
   *                         kPodOnly mode (callers may pass nullptr).
   * @param[out] out_handle  populated on success; contents are undefined on
   *                         failure.
   * @return true on success; false if the transport does not support loans
   *         for this (info, mode) pair, or temporarily cannot fulfill it.
   */
  virtual bool LoanSample(const TransportInfo& /*info*/,
                          size_t /*requested_size*/,
                          LoanMode /*mode*/,
                          const ibmw::transport::Context* /*ctx*/,
                          LoanedSampleHandle& /*out_handle*/) noexcept {
    return false;
  }

  /**
   * @brief Loan + serialize-into-loan in one driver call.
   *
   * Phase 7 §X1: promoted from iceoryx-only LoanSerializedSampleWithCtx()
   * into the virtual function family. Drivers that can serialize the typed
   * message directly into a loaned destination chunk (avoiding an
   * intermediate heap buffer) override this. The chunk is fully populated
   * with header + CDR bytes on success; the caller only needs to invoke
   * PublishLoaned(out_handle) to transmit.
   *
   * Default implementation returns false. Callers (typed wrappers in
   * iox_style_publisher.h) fall back to the regular Send() path on false.
   *
   * @param info             topic/type identification.
   * @param ctx              ctx-meta source (always required: this entry
   *                         point only makes sense for kCtxMetaThenPayload
   *                         layout).
   * @param msg_ptr          typed message pointer; the driver dispatches
   *                         to info.msg_type_support_ref to serialize.
   * @param[out] out_handle  populated on success.
   * @return true on success.
   */
  virtual bool LoanSerializedSample(const TransportInfo& /*info*/,
                                    const ibmw::transport::Context& /*ctx*/,
                                    const void* /*msg_ptr*/,
                                    LoanedSampleHandle& /*out_handle*/) noexcept {
    return false;
  }

  /**
   * @brief Publish a previously loaned sample. The driver takes ownership of
   * the underlying chunk; the handle becomes invalid after this call.
   *
   * Default implementation is a no-op (matches the false return of LoanSample).
   *
   * @param handle handle previously obtained from LoanSample(). For
   *               kCtxMetaThenPayload mode the driver fills the header just
   *               before publishing.
   */
  virtual void PublishLoaned(LoanedSampleHandle& /*handle*/) noexcept {}

  /**
   * @brief Release a loaned sample without publishing (e.g. on application
   * error after borrow). The handle becomes invalid after this call.
   *
   * Default implementation is a no-op.
   */
  virtual void ReleaseLoaned(LoanedSampleHandle& /*handle*/) noexcept {}
};

}  // namespace ibmw::runtime::core::transport

/**
 * @brief Declare a TransportDriver subclass's stable driver slot in one line.
 *
 * Usage (inside `class XxxTransportDriver : public TransportDriver { public: ... };`):
 *
 *   IBMW_DECLARE_DRIVER_SLOT("ibmw.transport.xxx")
 *
 * Expands to:
 *   - `static constexpr const char* kStableDriverId = "ibmw.transport.xxx";`
 *     so user pkgs can compute IBMW_DRIVER_ID(XxxTransportDriver) at compile time.
 *   - `ibmw_driver_slot_t Slot() const noexcept override { ... }` that returns
 *     ibmw::DriverSlotFromString(kStableDriverId).
 *
 * Keeping both members glued via this macro guarantees user-side compile-time
 * id == driver-side runtime id (skill R7 G6: cross-.so stability).
 */
#define IBMW_DECLARE_DRIVER_SLOT(StableIdLiteral)                              \
  static constexpr const char* kStableDriverId = StableIdLiteral;              \
  ibmw_driver_slot_t Slot() const noexcept override {                          \
    return ::ibmw::DriverSlotFromString(StableIdLiteral);                      \
  }
