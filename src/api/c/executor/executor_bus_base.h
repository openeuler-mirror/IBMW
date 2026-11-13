// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stddef.h>

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
   * @brief Size of this table in bytes (sizeof of the provider's struct).
   * Must be the first field. Consumers reject a table smaller than the
   * struct they were compiled against.
   */
  size_t struct_size;

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
