// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief String
 *
 */
typedef struct {
  /// Char buffer
  char* str;

  /// Length of char buffer
  size_t len;
} ibmw_string_t;

/**
 * @brief String array
 *
 */
typedef struct {
  /// String array
  ibmw_string_t* str_array;

  /// Length of string array
  size_t len;
} ibmw_string_array_t;

/**
 * @brief String view
 *
 */
typedef struct {
  /// Const char buffer
  const char* str;

  /// Length of char buffer
  size_t len;
} ibmw_string_view_t;

/**
 * @brief String view array
 *
 */
typedef struct {
  /// String view array
  const ibmw_string_view_t* str_array;

  /// Length of string view array
  size_t len;
} ibmw_string_view_array_t;

#ifdef __cplusplus
}
#endif
