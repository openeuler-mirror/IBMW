// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace ibmw::runtime::core::util {

void SetNameForCurrentThread(std::string_view thread_name);

void BindCpuForCurrentThread(const std::vector<uint32_t>& cpu_set);

void SetCpuSchedForCurrentThread(std::string_view sched);

}  // namespace ibmw::runtime::core::util
