// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#ifndef __cplusplus
#include <stdbool.h>
#endif

#include "c/transport/transport_context_base.h"
#include "c/util/function_base.h"
#include "c/util/string.h"
#include "c/util/type_support_base.h"

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Loan layout mode (mirrors C++ ibmw::runtime::core::transport::LoanMode).
 *
 * Phase 7 §7.3 §K-3.5: C ABI surface for the user-level zero-copy loan
 * fast lane. Values MUST stay in sync with LoanMode in
 * engine/transport/loaned_sample_handle.h (a static_assert in the C++
 * shim guards against drift). The underlying byte width MUST also match
 * (uint8_t on both sides) since the value lives inside a POD struct
 * exchanged across the C ABI by reinterpret_cast.
 */
typedef uint8_t ibmw_loan_mode_t;
#define IBMW_LOAN_MODE_POD_ONLY              ((ibmw_loan_mode_t)0)
#define IBMW_LOAN_MODE_CTX_META_THEN_PAYLOAD ((ibmw_loan_mode_t)1)
/* Driver completion discriminator; callers must not request this mode. */
#define IBMW_LOAN_MODE_INTERNAL_CTX_META_UNREPORTED ((ibmw_loan_mode_t)2)

/**
 * @brief C-ABI mirror of ibmw::runtime::core::transport::LoanedSampleHandle.
 *
 * Field layout, ordering and types MUST match the C++ struct exactly so
 * the implementation can reinterpret_cast between the two without copy.
 * static_assert lives in transport_access.h. Contents are filled in by
 * the loan() vtable entry on success and consumed by publish_loaned() /
 * release_loaned().
 *
 * On failure loan() returns a handle with user_payload_ptr == NULL and
 * source_driver == NULL. publish_loaned() / release_loaned() are no-ops
 * on such an empty handle.
 */
typedef struct {
  void* user_payload_ptr;     /**< start of user-writable payload region */
  size_t user_payload_size;   /**< size of payload region in bytes      */
  ibmw_loan_mode_t mode;      /**< wire-format layout                   */
  void* driver_token;         /**< driver-private bookkeeping (opaque)  */
  void* source_driver;        /**< driver routing tag (opaque)          */
} ibmw_loaned_sample_handle_t;

/**
 * @brief Operate struct for subscriber release callback
 * @note
 * Signature: void(*)()
 */
typedef struct {
  void (*invoker)(void* object);
  void (*relocator)(void* from, void* to);
  void (*destroyer)(void* object);
} ibmw_function_receiver_release_callback_ops_t;

/**
 * @brief Operate struct for subscriber callback
 * @note
 * Signature:
 * void (*)(
 *     const ibmw_transport_context_base_t* ctx_ptr,
 *     const void* msg,
 *     ibmw_function_base_t* release_callback)
 * Input 1: Const pointer to transport message handler context
 * Input 2: Const pointer to msg
 * Input 3: Release callback, which ops type is 'ibmw_function_receiver_release_callback_ops_t'
 * Raw C ABI callbacks must not throw across this function boundary. The public
 * C++ subscriber wrappers contain user exceptions before they reach this ABI.
 */
typedef struct {
  void (*invoker)(
      void* object,
      const ibmw_transport_context_base_t* ctx_ptr,
      const void* msg,
      ibmw_function_base_t* release_callback);
  void (*relocator)(void* from, void* to);
  void (*destroyer)(void* object);
} ibmw_function_receiver_callback_ops_t;

/**
 * @brief Operate struct for loaned subscriber callback
 * @note
 * Phase 7 §7.3 §U: receiver-side counterpart of the publisher loan fast
 * lane. Symmetric with ibmw_function_receiver_callback_ops_t but the msg
 * pointer is the user-payload region of a loaned (borrowed) sample, not
 * a deserialized message. Driver guarantees the payload pointer remains
 * valid until release_callback is invoked (or the user callback returns
 * if the driver chose RAII-style release).
 *
 * Signature:
 * void (*)(
 *     const ibmw_transport_context_base_t* ctx_ptr,
 *     const void* payload_ptr,
 *     size_t payload_size,
 *     ibmw_function_base_t* release_callback)
 * Input 1: Const pointer to transport message handler context (may carry
 *          driver-private metadata such as iceoryx ChunkHeader bits)
 * Input 2: Const pointer to user-writable payload region
 * Input 3: Payload size in bytes
 * Input 4: Release callback, which ops type is
 *          'ibmw_function_receiver_release_callback_ops_t'
 * Raw C ABI callbacks must not throw across this function boundary. The public
 * C++ LoanedSubscriber bridge catches user exceptions before invoking this ABI.
 */
typedef struct {
  void (*invoker)(
      void* object,
      const ibmw_transport_context_base_t* ctx_ptr,
      const void* payload_ptr,
      size_t payload_size,
      ibmw_function_base_t* release_callback);
  void (*relocator)(void* from, void* to);
  void (*destroyer)(void* object);
} ibmw_function_loaned_receiver_callback_ops_t;

/**
 * Maximum retained receive loans per subscription. Samples arriving at the
 * limit are dropped rather than deferred; later samples can be delivered once
 * a loan is released.
 */
typedef struct {
  size_t max_outstanding;
} ibmw_retained_loan_options_t;

/**
 * @brief Opaque retained-loan subscription control.
 *
 * Ownership is transferred to the caller by attach_loaned_retained(). The
 * caller must eventually invoke destroy(). stop() prevents new callbacks;
 * close() also waits up to timeout_ms for callbacks and retained loans to
 * quiesce. A false close result leaves the underlying subscription alive.
 */
typedef struct ibmw_loaned_subscription_base_t {
  void (*stop)(void* ctx);
  bool (*close)(void* ctx, uint64_t timeout_ms);
  void (*destroy)(void* ctx);
  void* ctx;
} ibmw_loaned_subscription_base_t;

/*
 * Appended sender/receiver entries preserve old-client/new-provider layout
 * only. These structs have no size/version field, so new APIs require matched
 * IBMW versions; new-client/old-provider compatibility cannot be guaranteed.
 */

/**
 * @brief Publisher interface
 *
 */
typedef struct {
  /**
   * @brief Size of this table in bytes (sizeof of the provider's struct).
   * Must be the first field. Consumers reject a table smaller than the
   * struct they were compiled against.
   */
  size_t struct_size;

  /**
   * @brief Function to register publish type
   * @note
   * Input 1: Implement pointer to publisher handle
   * Input 2: Msg type support
   * Return: Register result
   */
  bool (*register_send_type)(
      void* ctx, const ibmw_type_support_base_t* msg_type_support);

  /**
   * @brief Function to publish msg
   * @note
   * Input 1: Implement pointer to publisher handle
   * Input 2: Msg type name
   * Input 3: Const pointer to transport send context
   * Input 4: Const pointer to msg
   */
  void (*send)(
      void* ctx,
      ibmw_string_view_t msg_type,
      const ibmw_transport_context_base_t* ctx_ptr,
      const void* msg);

  /**
   * @brief Function to get topic for current publisher
   * @note
   * Input 1: Implement pointer to publisher handle
   * Return: Topic for current publisher
   */
  ibmw_string_view_t (*get_topic)(void* ctx);

  /**
   * @brief Function to merge subscribe context to publish context
   * @note
   * Input 1: Implement pointer to transport handle
   * Input 2: Transport message handler context
   * Input 3: Transport send context, for output
   */
  void (*merge_subscribe_context_to_publish_context)(
      void* ctx,
      const ibmw_transport_context_base_t* subscribe_ctx_ptr,
      const ibmw_transport_context_base_t* publish_ctx_ptr);

  /**
   * @brief Function to borrow a loaned sample for zero-copy publishing.
   * @note
   * Phase 7 §7.3 §K-3.5: user-level zero-copy fast lane. The dispatcher
   * deliberately bypasses the publish filter chain and ctx-meta KV
   * writes (pub_seq / pub_ts) on this path. Caller must check
   * out_handle->user_payload_ptr for non-NULL before writing.
   * Input 1: Implement pointer to publisher handle
   * Input 2: Msg type name
   * Input 3: Requested user payload size in bytes
   * Input 4: Wire-format layout
   * Input 5: Output handle (zeroed on failure)
   */
  void (*loan)(
      void* ctx,
      ibmw_string_view_t msg_type,
      size_t requested_size,
      ibmw_loan_mode_t mode,
      ibmw_loaned_sample_handle_t* out_handle);

  /**
   * @brief Function to publish a previously loaned sample.
   * @note
   * Driver consumes the handle and clears its source_driver field on
   * completion. Calling on an empty handle (source_driver == NULL) is a
   * no-op.
   * Input 1: Implement pointer to publisher handle
   * Input 2: Handle previously returned by loan()
   */
  void (*publish_loaned)(
      void* ctx,
      ibmw_loaned_sample_handle_t* handle);

  /**
   * @brief Function to release a previously loaned sample without publishing.
   * @note
   * Used by RAII wrappers on the abort/discard path. No-op on an empty
   * handle.
   * Input 1: Implement pointer to publisher handle
   * Input 2: Handle previously returned by loan()
   */
  void (*release_loaned)(
      void* ctx,
      ibmw_loaned_sample_handle_t* handle);

  /// Implement pointer
  void* ctx;

  /**
   * @brief Register a transport-selected raw POD loan topic.
   * @note
   * This entry is appended after the legacy fields. It is legal only during
   * dispatcher Init. New callers require a provider built from the same IBMW
   * version because this vtable has no size/version field.
   * Input 1: Implement pointer to publisher handle
   * Input 2: Msg type name
   * Input 3: Nonzero initial payload size hint
   * Input 4: Nonzero power-of-two payload alignment
   * Return: Registration result
   */
  bool (*register_loaned_pod_topic)(
      void* ctx,
      ibmw_string_view_t msg_type,
      size_t initial_size_hint,
      size_t alignment);
} ibmw_transport_sender_base_t;

/**
 * @brief Subscriber interface
 *
 */
typedef struct {
  /**
   * @brief Size of this table in bytes (sizeof of the provider's struct).
   * Must be the first field. Consumers reject a table smaller than the
   * struct they were compiled against.
   */
  size_t struct_size;

  /**
   * @brief Function to subscribe msg
   * @note
   * Input 1: Implement pointer to subscriber handle
   * Input 2: Msg type support
   * Input 3: Msg callback, which ops type is 'ibmw_function_receiver_callback_ops_t'
   * Return: Subscribe result
   */
  bool (*on_message)(
      void* ctx,
      const ibmw_type_support_base_t* msg_type_support,
      ibmw_function_base_t* callback);

  /**
   * @brief Function to subscribe loaned msg (zero-copy receive fast lane)
   * @note
   * Phase 7 §7.3 §U: receiver-side counterpart of sender::loan. Driver
   * delivers the user-payload pointer of a borrowed sample directly to
   * the callback without deserialization. Drivers that cannot provide
   * zero-copy receive (e.g. dds, mqtt, tcp) MUST leave this function
   * pointer NULL; the dispatcher checks for NULL and returns failure. This
   * zero-copy API does not silently subscribe through on_message instead.
   *
   * The callback ops type is 'ibmw_function_loaned_receiver_callback_ops_t'.
   * msg_type_support is passed for symmetry with on_message and lets the
   * driver validate the topic-to-type binding; payload size is decided
   * by the publisher loan() call, not by the type system.
   *
   * Input 1: Implement pointer to subscriber handle
   * Input 2: Msg type support
   * Input 3: Loaned msg callback
   * Return:  Subscribe result (false if driver does not support zero-copy
   *          receive or the type binding is rejected)
   */
  bool (*attach_loaned)(
      void* ctx,
      const ibmw_type_support_base_t* msg_type_support,
      ibmw_function_base_t* callback);

  /**
   * @brief Function to get topic for current subscriber
   * @note
   * Input 1: Implement pointer to subscriber handle
   * Return: Topic for current subscriber
   */
  ibmw_string_view_t (*get_topic)(void* ctx);

  /// Implement pointer
  void* ctx;

  /**
   * @brief Subscribe with transferable receive-loan ownership.
   *
   * This entry is appended after the legacy fields and preserves the
   * old-client/new-provider layout only. Existing structs have no size/version
   * field, so new-client/old-provider compatibility is not guaranteed and the
   * retained API requires matched IBMW versions. On success,
   * out_subscription receives an owned control handle. The callback receives
   * a release callback whose ownership may be moved beyond callback return.
   */
  bool (*attach_loaned_retained)(
      void* ctx,
      const ibmw_type_support_base_t* msg_type_support,
      ibmw_function_base_t* callback,
      ibmw_retained_loan_options_t options,
      ibmw_loaned_subscription_base_t** out_subscription);
} ibmw_transport_receiver_base_t;

/**
 * @brief Transport bus interface
 *
 */
typedef struct {
  /**
   * @brief Size of this table in bytes (sizeof of the provider's struct).
   * Must be the first field. Consumers reject a table smaller than the
   * struct they were compiled against.
   */
  size_t struct_size;

  /**
   * @brief Function to get publisher for a topic
   * Input 1: Implement pointer to transport bus
   * Input 2: Topic name
   * Output: Publisher handle
   */
  const ibmw_transport_sender_base_t* (*get_sender)(
      void* ctx, ibmw_string_view_t topic);

  /**
   * @brief Function to get subscriber for a topic
   * Input 1: Implement pointer to transport bus
   * Input 2: Topic name
   * Output: Subscriber handle
   */
  const ibmw_transport_receiver_base_t* (*get_receiver)(
      void* ctx, ibmw_string_view_t topic);

  /**
   * @brief Function to merge subscribe context to publish context
   * @note
   * Input 1: Implement pointer to transport handle
   * Input 2: Transport message handler context
   * Input 3: Transport send context, for output
   */
  void (*merge_subscribe_context_to_publish_context)(
      void* ctx,
      const ibmw_transport_context_base_t* subscribe_ctx_ptr,
      const ibmw_transport_context_base_t* publish_ctx_ptr);

  /// Implement pointer
  void* ctx;
} ibmw_transport_handle_base_t;

#ifdef __cplusplus
}
#endif
