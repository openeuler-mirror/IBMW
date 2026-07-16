// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stdint.h>

#include "c/util/function_base.h"
#include "c/util/string.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Operate struct for executor task
 * @note
 * Signature: void(*)()
 */
typedef struct {
  void (*invoker)(void* object);
  void (*relocator)(void* from, void* to);
  void (*destroyer)(void* object);
} ibmw_function_executor_task_ops_t;

/**
 * @brief Executor interface
 *
 */
typedef struct {
  /// Function to get executor type
  ibmw_string_view_t (*type)(void* ctx);

  /// Function to get executor name
  ibmw_string_view_t (*name)(void* ctx);

  /// Function to get executor thread safety
  bool (*is_thread_safe)(void* ctx);

  /// Function to determine if in current executor
  bool (*is_in_current_executor)(void* ctx);

  /// Function to determine if timer schedule is supported
  bool (*supports_timed_dispatch)(void* ctx);

  /**
   * @brief Function to execute task
   * @note
   * Input 1: Implement pointer to executor handle
   * Input 2: Task, which ops type is 'ibmw_function_executor_task_ops_t'
   */
  void (*dispatch)(void* ctx, ibmw_function_base_t* task);

  /**
   * @brief Function to get current timestamp
   * @note
   * Input 1: Implement pointer to executor handle
   * Output: Current nanosecond timestamp, since 1970-01-01 00:00:00 UTC
   */
  uint64_t (*now)(void* ctx);

  /**
   * @brief Function to execute task at a time point
   * @note
   * Input 1: Implement pointer to executor handle
   * Input 2: Timestamp, nanosecond since 1970-01-01 00:00:00 UTC
   * Input 3: Task, which ops type is 'ibmw_function_executor_task_ops_t'
   */
  void (*dispatch_at_ns)(void* ctx, uint64_t tp, ibmw_function_base_t* task);

  /// Implement pointer
  void* ctx;
} ibmw_executor_base_t;

#ifdef __cplusplus
}
#endif
