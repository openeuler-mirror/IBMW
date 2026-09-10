// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <functional>

#include <boost/asio/ip/basic_endpoint.hpp>

namespace ibmw::common::net {

struct AsioEndpointHash {
  template <typename Protocol>
  std::size_t operator()(
      const boost::asio::ip::basic_endpoint<Protocol>& endpoint) const noexcept {
    std::size_t seed = 0;
    const auto combine = [&seed](std::size_t value) {
      seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
    };

    const auto address = endpoint.address();
    combine(address.is_v4() ? 4U : 6U);
    if (address.is_v4()) {
      for (const auto byte : address.to_v4().to_bytes()) combine(byte);
    } else {
      const auto address_v6 = address.to_v6();
      for (const auto byte : address_v6.to_bytes()) combine(byte);
      combine(std::hash<unsigned long>{}(address_v6.scope_id()));
    }
    combine(std::hash<unsigned short>{}(endpoint.port()));
    return seed;
  }
};

}  // namespace ibmw::common::net
