// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ibmw::extensions::grpc_extension::grpc {

constexpr size_t kGrpcMessagePrefixSize = 5;
// The prefix stores the message length in 32 bits.
constexpr size_t kMaxGrpcMessageLength = UINT32_MAX;

struct GrpcMessagePrefix {
  uint8_t compression_flag;
  uint32_t message_length;
};

std::optional<GrpcMessagePrefix> ParseGrpcMessagePrefix(std::string_view message);

std::string EncodeGrpcMessagePrefix(const GrpcMessagePrefix& prefix);

}  // namespace ibmw::extensions::grpc_extension::grpc
