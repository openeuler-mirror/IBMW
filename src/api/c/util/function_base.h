// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifndef IBMW_FUNCTION_LOCAL_BUF_SIZE
  /**
   * Small-buffer optimization size for ibmw_function_base_t.
   *
   * 48 bytes (6 pointers on 64-bit) covers most closures that capture up to
   * ~5 pointer-sized values, avoiding a heap allocation. Increase this if
   * profiling shows frequent fallback to the heap path in Function<Ops>.
   */
  #define IBMW_FUNCTION_LOCAL_BUF_SIZE (6 * sizeof(void*))
#endif

/**
 * @brief Function
 *
 */
typedef struct {
  /// Buffer to store closure data
  unsigned char object_buf[IBMW_FUNCTION_LOCAL_BUF_SIZE];

  /**
   * @brief Const pointer to an operate struct
   * @note
   * An operate struct has 3 necessary members:
   * 1. invoker: Function to invoke the closure, the signature of it is the signature the closure
   * 2. relocator: Function to relocator the closure, signature: void(*relocator)(void* from, void* to);
   * 3. destroyer: Function to destroyer the closure, signature: void(*destroyer)(void* object);
   *
   * eg:
   * typedef struct {
   *   int (*invoker)(void* object, int val);
   *   void (*relocator)(void* from, void* to);
   *   void (*destroyer)(void* object);
   * } test_ops_t;
   *
   * This is a simple example of an operate struct that represents a simple closure.
   * It's invoke signature is <int(*)(int)>.
   */
  const void* ops;
} ibmw_function_base_t;

#ifdef __cplusplus
}
#endif
