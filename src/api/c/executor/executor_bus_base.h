// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/executor/executor_base.h"
#include "c/util/string.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Executor bus interface
 *
 */
typedef struct {
  /**
   * @brief Function to get executor
   * @note
   * Input 1: Implement pointer to executor bus handle
   * Input 2: Executor name
   */
  const ibmw_executor_base_t* (*get_executor)(void* ctx, ibmw_string_view_t executor_name);

  /// Implement pointer
  void* ctx;
} ibmw_executor_bus_base_t;

#ifdef __cplusplus
}
#endif
