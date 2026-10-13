// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <chrono>
#include <string>
#include <vector>
#include <boost/asio/ip/tcp.hpp>

namespace ibmw::discovery {
// Isolate blocking system name resolution from the unloadable discovery DSO.
std::vector<boost::asio::ip::tcp::endpoint> ResolveHttpEndpoints(
    const std::string& host, const std::string& port,
    std::chrono::steady_clock::time_point deadline);
}  // namespace ibmw::discovery
