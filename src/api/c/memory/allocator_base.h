// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
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
