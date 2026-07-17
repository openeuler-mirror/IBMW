// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/util/function_base.h"
#include "c/util/string.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Operate struct for parameter value release callback
 * @note Signature: void(*)()
 *
 */
typedef struct {
  void (*invoker)(void* object);
  void (*relocator)(void* from, void* to);
  void (*destroyer)(void* object);
} ibmw_function_parameter_val_release_callback_ops_t;

/// Parameter value with release callback
typedef struct {
  /// parameter value
  ibmw_string_view_t parameter_val;

  /**
   * @brief release callback
   * @note ops type is ibmw_function_parameter_val_release_callback_ops_t
   */
  ibmw_function_base_t* release_callback;
} ibmw_parameter_val_view_holder_t;

/// Parameter handle
typedef struct {
  /**
   * @brief Get parameter reference
   * @note
   * Input 1: Implement pointer to parameter handle
   * Input 2: Parameter key
   * Output: Parameter value with release callback
   */
  ibmw_parameter_val_view_holder_t (*get_parameter)(void* ctx, ibmw_string_view_t key);

  /**
   * @brief Set parameter
   * @note
   * Input 1: Implement pointer to parameter handle
   * Input 2: Parameter key
   * Input 3: Parameter value
   */
  void (*set_parameter)(void* ctx, ibmw_string_view_t key, ibmw_string_view_t val);

  /// Implement pointer
  void* ctx;
} ibmw_parameter_handle_base_t;

#ifdef __cplusplus
}
#endif
