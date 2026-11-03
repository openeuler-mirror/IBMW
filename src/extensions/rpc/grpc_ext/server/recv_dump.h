// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ibmw::extensions::grpc_extension::server {

// Trace dump of received bytes (EXT-010): hex on the first line and printable
// characters on the second, limited to the first max_bytes bytes.
inline std::string FormatRecvDump(const uint8_t* data, size_t size, size_t max_bytes = 64) {
  static constexpr char kHex[] = "0123456789abcdef";
  const size_t shown = size < max_bytes ? size : max_bytes;
  std::string hex;
  std::string text;
  hex.reserve(shown * 3);
  text.reserve(shown * 3);
  for (size_t i = 0; i < shown; ++i) {
    hex += kHex[data[i] >> 4];
    hex += kHex[data[i] & 0xf];
    hex += ' ';
    text += (data[i] >= 0x20 && data[i] < 0x7f) ? static_cast<char>(data[i]) : '.';
    text += "  ";
  }
  std::string out = std::move(hex);
  out += '\n';
  out += text;
  if (size > shown) {
    out += "\n... ";
    out += std::to_string(size - shown);
    out += " more bytes";
  }
  return out;
}

}  // namespace ibmw::extensions::grpc_extension::server
