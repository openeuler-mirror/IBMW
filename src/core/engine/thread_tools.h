// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ibmw::runtime::core::util {

void SetNameForCurrentThread(std::string_view thread_name);

void BindCpuForCurrentThread(const std::vector<uint32_t>& cpu_set);

void SetCpuSchedForCurrentThread(std::string_view sched);

// Alternate signal stack size for the crash handler (symbolizing a trace
// needs far more than SIGSTKSZ). Only touched pages use memory.
inline constexpr size_t kCrashAltStackSize = 8 * 1024 * 1024;

// Switched on while the crash handler is installed.
void EnableAltSignalStacks(bool enable);

// Gives the calling thread its own alternate signal stack when enabled, so a
// stack overflow on it still reaches the crash handler (sigaltstack is per
// thread). Released when the thread exits; no-op if it already has one.
void EnsureAltSignalStackForCurrentThread();

}  // namespace ibmw::runtime::core::util
