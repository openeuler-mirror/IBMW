// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#if defined _WIN32 || defined __CYGWIN__
  #ifdef __GNUC__
    #define IBMW_CORE_EXTENSION_EXPORT __attribute__((dllexport))
  #else
    #define IBMW_CORE_EXTENSION_EXPORT __declspec(dllexport)
  #endif
#else
  #define IBMW_CORE_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
}
#endif
