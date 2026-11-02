// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/basic_endpoint.hpp>

namespace ibmw::common::net {

/**
 * @brief Extract the authority ("host:port") part of an address string
 * @note Drops an optional "scheme://" prefix and anything starting at the
 * first '/', '?' or '#'. Does not allocate.
 */
inline std::string_view ExtractUrlAuthority(std::string_view url) noexcept {
  if (auto pos = url.find("://"); pos != std::string_view::npos) url.remove_prefix(pos + 3);
  return url.substr(0, url.find_first_of("/?#"));
}

/**
 * @brief Parse "host:port" / "[ipv6]:port" into an IP endpoint without throwing
 * @note Host must be a numeric IPv4/IPv6 address (no name resolution). Port must
 * be a decimal number in [1, 65535]. Returns std::nullopt on any malformed input.
 */
template <typename Protocol>
std::optional<boost::asio::ip::basic_endpoint<Protocol>> ParseIpEndpoint(
    std::string_view authority) noexcept {
  std::string_view host;
  std::string_view port;
  if (!authority.empty() && authority.front() == '[') {
    auto close = authority.find(']');
    if (close == std::string_view::npos || close + 1 >= authority.size() ||
        authority[close + 1] != ':')
      return std::nullopt;
    host = authority.substr(1, close - 1);
    port = authority.substr(close + 2);
  } else {
    auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) return std::nullopt;
    host = authority.substr(0, colon);
    port = authority.substr(colon + 1);
    // Unbracketed IPv6 with a port is ambiguous ("::1:80"); require brackets.
    if (host.find(':') != std::string_view::npos) return std::nullopt;
  }
  if (host.empty() || port.empty()) return std::nullopt;

  uint32_t port_num = 0;
  auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), port_num);
  if (ec != std::errc() || ptr != port.data() + port.size() ||
      port_num == 0 || port_num > 65535)
    return std::nullopt;

  // INET6_ADDRSTRLEN with scope id fits easily; longer strings are invalid.
  char host_buf[64];
  if (host.size() >= sizeof(host_buf)) return std::nullopt;
  host.copy(host_buf, host.size());
  host_buf[host.size()] = '\0';

  boost::system::error_code addr_ec;
  auto addr = boost::asio::ip::make_address(host_buf, addr_ec);
  if (addr_ec) return std::nullopt;

  return boost::asio::ip::basic_endpoint<Protocol>(addr, static_cast<uint16_t>(port_num));
}

}  // namespace ibmw::common::net
