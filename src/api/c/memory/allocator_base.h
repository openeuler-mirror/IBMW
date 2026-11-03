// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  /**
   * @brief Size of this table in bytes (sizeof of the provider's struct).
   * Must be the first field. Consumers reject a table smaller than the
   * struct they were compiled against.
   */
  size_t struct_size;

  /**
   * @brief Get thread local buf from core
   *
   */
  void* (*get_thread_local_buf)(void* ctx, size_t buf_size);

  /// Implement pointer
  void* ctx;
} ibmw_allocator_base_t;

#ifdef __cplusplus
}
#endif
