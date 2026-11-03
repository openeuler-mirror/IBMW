// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stddef.h>

#include "c/util/string.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configurator interface
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
   * @brief Function to get config file path
   * @note
   * 1. If the module cfg file path is specified in the module cfg node, the
   * specified file path will be returned here.
   * 2. If the module's cfg is written in the root cfg file, the framework will
   * generate a temporary yaml configuration file containing module cfg info for
   * this module, and the path to this temporary configuration file will be
   * returned here.
   */
  ibmw_string_view_t (*config_file_path)(void* ctx);

  /// Implement pointer
  void* ctx;
} ibmw_configurator_base_t;

#ifdef __cplusplus
}
#endif
