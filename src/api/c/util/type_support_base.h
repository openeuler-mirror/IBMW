// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stdbool.h>

#include "c/util/buffer_base.h"
#include "c/util/string.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Type support interface
 *
 */
typedef struct {
  /**
   * @brief Get type name
   * @note
   * Input 1: Implement pointer
   * Output: Type name
   */
  ibmw_string_view_t (*type_name)(void* ctx);

  /**
   * @brief Function to create msg
   * @note
   * Input 1: Implement pointer
   * Output: Pointer to msg
   */
  void* (*create)(void* ctx);

  /**
   * @brief Function to destroy msg
   * @note
   * Input 1: Implement pointer
   * Input 2: Pointer to msg
   */
  void (*destroy)(void* ctx, void* msg);

  /**
   * @brief Function to copy msg
   * @note
   * Input 1: Implement pointer
   * Input 2: Pointer to the msg to be copied from
   * Input 3: Pointer to the msg to be copied to
   */
  void (*copy)(void* ctx, const void* from, void* to);

  /**
   * @brief Function to move msg
   * @note
   * Input 1: Implement pointer
   * Input 2: Pointer to the msg to be moved from
   * Input 3: Pointer to the msg to be moved to
   */
  void (*move)(void* ctx, void* from, void* to);

  /**
   * @brief Function to serialize the msg
   * @note
   * Input 1: Implement pointer
   * Input 2: Serialization type, eg: cdr/json/ros2
   * Input 3: Pointer to the msg to be serialized
   * Input 4: Pointer to allocator for buffer array
   * Input 5: Pointer to the buffer array
   * Output: Serialization result
   */
  bool (*serialize)(
      void* ctx,
      ibmw_string_view_t serialization_type,
      const void* msg,
      const ibmw_buffer_array_allocator_t* allocator,
      ibmw_buffer_array_t* buffer_array);

  /**
   * @brief Function to deserialize the msg
   * @note
   * Input 1: Implement pointer
   * Input 2: Serialization type, eg: cdr/json/ros2
   * Input 3: Buffer array
   * Input 4: Pointer to the msg to be deserialized
   * Output: Deserialization result
   */
  bool (*deserialize)(
      void* ctx,
      ibmw_string_view_t serialization_type,
      ibmw_buffer_array_view_t buffer_array_view,
      void* msg);

  /**
   * @brief Number of serialization types supported
   * @note
   * Input 1: Implement pointer
   * Output: Number of serialization types supported
   */
  size_t (*serialization_types_supported_num)(void* ctx);

  /**
   * @brief List of serialization types supported
   * @note
   * Input 1: Implement pointer
   * Output: Name array. The length of this array is defined by serialization_types_supported_num
   */
  const ibmw_string_view_t* (*serialization_types_supported_list)(void* ctx);

  /**
   * @brief For custom type support
   * @note
   * Input 1: Implement pointer
   * Output: Custom type support
   */
  const void* (*custom_type_support_ptr)(void* ctx);

  /// Implement pointer
  void* ctx;
} ibmw_type_support_base_t;

#ifdef __cplusplus
}
#endif
