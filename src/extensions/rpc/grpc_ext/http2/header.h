// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_map>

namespace ibmw::extensions::grpc_extension::http2 {

using Headers = std::unordered_map<std::string, std::string>;
using Trailers = std::unordered_map<std::string, std::string>;

}  // namespace ibmw::extensions::grpc_extension::http2
