// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdio>

#include <unistd.h>
namespace ibmw::runtime::core::logger {

inline bool Fsync(FILE *fp) {
  return ::fsync(fileno(fp)) == 0;
}

// 文件夹分隔符定义
#if !defined(FOLDER_SEPS)
  #define FOLDER_SEPS "/"
#endif

inline constexpr const char folder_seps[] = FOLDER_SEPS;

}  // namespace ibmw::runtime::core::logger
