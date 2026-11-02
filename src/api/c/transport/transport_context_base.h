// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "c/util/string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Some frame fields. Users should not directly modify these fields

#define IBMW_TRANSPORT_CONTEXT_KEY_PREFIX "ibmw-"

/// eg: json/cdr
#define IBMW_TRANSPORT_CONTEXT_KEY_SERIALIZATION_TYPE "ibmw-serialization_type"

/// eg: local/mqtt/http
#define IBMW_TRANSPORT_CONTEXT_KEY_DRIVER "ibmw-driver"

/// eg: http://127.0.0.1:50040;tcp://127.0.0.1:50060;udp://127.0.0.1:50040
#define IBMW_TRANSPORT_CONTEXT_KEY_TO_ADDR "ibmw-to_addr"

/// publish sequence for one topic
#define IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ "ibmw-pub_seq"

/// publish timestamp for one topic
#define IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP "ibmw-pub_ts"

typedef enum {
  IBMW_TRANSPORT_PUBLISHER_CONTEXT = 0,
  IBMW_TRANSPORT_SUBSCRIBER_CONTEXT = 1,
} ibmw_transport_context_type_t;

/**
 * @brief Transport context operate interface
 *
 */
typedef struct {
  /**
   * @brief Function to check if the context is used already
   *
   */
  bool (*check_used)(void* ctx);

  /**
   * @brief Function to set the context to used status
   *
   */
  void (*set_used)(void* ctx);

  /**
   * @brief Function to get context type
   *
   */
  ibmw_transport_context_type_t (*get_type)(void* ctx);

  /**
   * @brief Function to get kv meta data
   *
   */
  ibmw_string_view_t (*get_meta_val)(void* ctx, ibmw_string_view_t key);

  /**
   * @brief Function to set kv meta data
   *
   */
  void (*set_meta_val)(void* ctx, ibmw_string_view_t key, ibmw_string_view_t val);

  /**
   * @brief Function to get all meta keys
   *
   */
  ibmw_string_view_array_t (*get_meta_keys)(void* ctx);

  /**
   * @brief Function to get all meta keys and values
   *
   */
  ibmw_string_view_array_t (*get_meta_key_vals)(void* ctx);

} ibmw_transport_context_base_ops_t;

/**
 * @brief Transport context interface
 *
 */
typedef struct {
  /// Const pointer to transport context operate interface
  const ibmw_transport_context_base_ops_t* ops;

  /// Implement pointer
  void* ctx;
} ibmw_transport_context_base_t;

#ifdef __cplusplus
}
#endif
