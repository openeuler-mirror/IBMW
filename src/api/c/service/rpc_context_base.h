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

#define IBMW_RPC_CONTEXT_KEY_PREFIX "ibmw-"

/// eg: driver://uri_defined_by_driver
#define IBMW_RPC_CONTEXT_KEY_TO_ADDR "ibmw-to_addr"

/// eg: json/cdr
#define IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE "ibmw-serialization_type"

/// eg: cdr:/example.ExampleService/GetFooData
#define IBMW_RPC_CONTEXT_KEY_FUNCTION_NAME "ibmw-function_name"

/// eg: local/mqtt/http
#define IBMW_RPC_CONTEXT_KEY_DRIVER "ibmw-driver"

/// eg: 1234567890
#define IBMW_RPC_CONTEXT_KEY_REQUEST_ID "ibmw-request_id"

/// eg: 1234567890
#define IBMW_RPC_CONTEXT_KEY_RESPONSE_ID "ibmw-response_id"

typedef enum {
  IBMW_RPC_CLIENT_CONTEXT = 0,
  IBMW_RPC_SERVER_CONTEXT = 1,
} ibmw_rpc_context_type_t;

/**
 * @brief Service context operate interface
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
  ibmw_rpc_context_type_t (*get_type)(void* ctx);

  /**
   * @brief Function to get the timeout(ns)
   *
   */
  uint64_t (*get_timeout_ns)(void* ctx);

  /**
   * @brief Function to set the timeout(ns)
   *
   */
  void (*set_timeout_ns)(void* ctx, uint64_t timeout);

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
   * The array is owned by the context and stays valid until its meta data
   * changes or it is destroyed. Concurrent calls are safe while the meta data
   * is not modified.
   */
  ibmw_string_view_array_t (*get_meta_keys)(void* ctx);

  /**
   * @brief Function to get all meta keys and values
   *
   * The array is owned by the context and stays valid until its meta data
   * changes or it is destroyed. Concurrent calls are safe while the meta data
   * is not modified.
   */
  ibmw_string_view_array_t (*get_meta_key_vals)(void* ctx);

} ibmw_rpc_context_base_ops_t;

/**
 * @brief Service context interface
 *
 */
typedef struct {
  /// Const pointer to service context operate interface
  const ibmw_rpc_context_base_ops_t* ops;

  /// Implement pointer
  void* ctx;
} ibmw_service_context_base_t;

#ifdef __cplusplus
}
#endif
