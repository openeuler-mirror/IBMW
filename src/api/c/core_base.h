// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stdint.h>

#include "c/memory/allocator_base.h"
#include "c/transport/transport_handle_base.h"
#include "c/configurator/configurator_base.h"
#include "c/executor/executor_bus_base.h"
#include "c/logger/logger_base.h"
#include "c/parameter/parameter_handle_base.h"
#include "c/service/rpc_handle_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IBMW module info
 * @note Version standard see https://semver.org/
 */
typedef struct {
  ibmw_string_view_t module_name;

  uint32_t major_version;

  uint32_t minor_version;

  uint32_t patch_version;

  uint32_t build_version;

  ibmw_string_view_t author;

  ibmw_string_view_t description;

} ibmw_module_info_t;

/**
 * @brief Current ABI version for ibmw_core_base_t.
 *
 * Increment this when the core C ABI, including handles reachable from
 * ibmw_core_base_t, changes. Plugins compiled against a different ABI version
 * can detect the mismatch before calling an incompatible function pointer.
 */
#define IBMW_CORE_ABI_VERSION 2

/**
 * @brief IBMW runtime framework interface
 *
 */
typedef struct {
  /**
   * @brief ABI version — must always be the first field.
   *
   * Set to IBMW_CORE_ABI_VERSION when the struct is created.
   * Consumers should check this value before accessing any other field.
   */
  uint32_t abi_version;

  /**
   * @brief Function to get module info for current core instance
   * @note
   * Parameter definition:
   * Input 1: Pointer to ctx
   * Output: Module info
   */
  ibmw_module_info_t (*info)(void* ctx);

  /**
   * @brief Function to get configurator handle
   * @note
   * Parameter definition:
   * Input 1: Pointer to ctx
   * Output: Configurator handle
   */
  const ibmw_configurator_base_t* (*configurator)(void* ctx);

  /**
   * @brief Function to get executor bus handle
   * @note
   * Parameter definition:
   * Input 1: Pointer to ctx
   * Output: Executor bus handle
   */
  const ibmw_executor_bus_base_t* (*executor_bus)(void* ctx);

  /**
   * @brief Function to get logger handle
   * @note
   * Parameter definition:
   * Input 1: Pointer to ctx
   * Output: Logger handle
   */
  const ibmw_logger_base_t* (*logger)(void* ctx);

  /**
   * @brief Function to get allocator handle
   * @note
   * Parameter definition:
   * Input 1: Pointer to ctx
   * Output: Allocator handle
   */
  const ibmw_allocator_base_t* (*memory_handle)(void* ctx);

  /**
   * @brief Function to get service handle
   * @note
   * Parameter definition:
   * Input 1: Pointer to ctx
   * Output: Service handle
   */
  const ibmw_service_handle_base_t* (*service_handle)(void* ctx);

  /**
   * @brief Function to get transport handle
   * @note
   * Parameter definition:
   * Input 1: Pointer to ctx
   * Output: Transport handle
   */
  const ibmw_transport_handle_base_t* (*transport_handle)(void* ctx);

  /**
   * @brief Function to get parameter handle
   * @note
   * Parameter definition:
   * Input 1: Pointer to ctx
   * Output: Parameter handle
   */
  const ibmw_parameter_handle_base_t* (*parameter_handle)(void* ctx);

  /// Implement pointer
  void* ctx;
} ibmw_core_base_t;

#ifdef __cplusplus
}
#endif
