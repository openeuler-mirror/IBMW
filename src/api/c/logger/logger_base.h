// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Log level definition
typedef enum {
  IBMW_LOG_LEVEL_TRACE = 0,
  IBMW_LOG_LEVEL_DEBUG = 1,
  IBMW_LOG_LEVEL_INFO = 2,
  IBMW_LOG_LEVEL_WARN = 3,
  IBMW_LOG_LEVEL_ERROR = 4,
  IBMW_LOG_LEVEL_FATAL = 5,
  IBMW_LOG_LEVEL_OFF = 6,
} ibmw_log_level_t;

/**
 * @brief Logger interface
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
   * @brief Function to get log level
   *
   */
  ibmw_log_level_t (*get_log_level)(void* ctx);

  /**
   * @brief Function to log
   * @note
   * Input 1: Implement pointer to logger handle
   * Input 2: Log level
   * Input 3: Code line
   * Input 4: Code file name
   * Input 5: Code function name
   * Input 6: Log data
   * Input 7: Log data size
   */
  void (*log)(void* ctx, ibmw_log_level_t lvl, uint32_t line,
              const char* file_name, const char* function_name,
              const char* log_data, size_t log_data_size);

  /// Implement pointer
  void* ctx;
} ibmw_logger_base_t;

#ifdef __cplusplus
}
#endif
