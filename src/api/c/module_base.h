// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/core_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IBMW module interface
 * Users need to implement these interfaces
 */
typedef struct {
  /**
   * @brief Function to get module info
   * @note
   * 1. Modules should return the same info throughout their entire lifecycle.
   *
   * Parameter definition:
   * Input 1: Pointer to module ctx
   * Output: Module info
   */
  ibmw_module_info_t (*info)(void* ctx);

  /**
   * @brief Function to load/initialize module
   * @note
   * 1. The framework will call the setup method of module after loading it.
   * 2. The load order of modules is undefined.
   * 3. The framework ensures that the setup method is executed in the main thread,
   *    and the module should not block this method for too long.
   * 4. Some api (eg: RPC Registration/Transport Registration) can only be called in the
   *    setup method.
   *
   * Parameter definition:
   * Input 1: Pointer to module ctx
   * Input 2: IBMW core interface pointer, modules call the function of the framework
   *           through this handle
   * Output: Setup result
   */
  bool (*setup)(void* ctx, const ibmw_core_base_t* core);

  /**
   * @brief Function to start module
   * @note
   * 1. The framework will call the run method of module after loading all modules.
   * 2. The start sequence of the modules will be consistent with the load
   *    sequence of the modules.
   * 3. The framework ensures that the run method is executed in the main thread,
   *    and the module should not block this method for too long.
   * 4. Some api (eg: RPC invoke/Transport send) can only be called after the run method.
   *
   * Parameter definition:
   * Input 1: Pointer to module ctx
   * Output: Run result
   */
  bool (*run)(void* ctx);

  /**
   * @brief Function to stop module
   * @note
   * 1. The framework will call the stop method of module when the framework stops running.
   * 2. The stop order of modules will be the opposite to the load order of modules.
   * 3. The framework ensures that the stop method is executed in the main thread,
   *    and the module should not block this method for too long.
   * 4. All api of framework should not be called after the stop method.
   *
   * Parameter definition:
   * Input 1: Pointer to module ctx
   */
  void (*stop)(void* ctx);

  /// Implement pointer
  void* ctx;
} ibmw_module_base_t;

#ifdef __cplusplus
}
#endif
