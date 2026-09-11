// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#if defined _WIN32 || defined __CYGWIN__
  #ifdef __GNUC__
    #define IBMW_TRANSPORT_EXPORT __attribute__((dllexport))
  #else
    #define IBMW_TRANSPORT_EXPORT __declspec(dllexport)
  #endif
#else
  #define IBMW_TRANSPORT_EXPORT __attribute__((visibility("default")))
#endif
