// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace ibmw::extensions::grpc_extension::grpc {

constexpr std::chrono::nanoseconds kDefaultTimeout = std::chrono::nanoseconds::max();

std::optional<std::chrono::nanoseconds> ParseTimeout(const std::string& timeout_str);

std::string FormatTimeout(std::chrono::nanoseconds timeout);

}  // namespace ibmw::extensions::grpc_extension::grpc
