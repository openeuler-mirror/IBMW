// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "base/buffer_util.h"
#include "iceoryx_ext/internal/payload_size.h"

namespace ibmw::extensions::iceoryx_extension::internal {
namespace {

std::array<char, 8> PayloadWithEncodedSize(uint32_t encoded_size) {
  std::array<char, 8> payload{};
  common::util::SetBufFromUint32(payload.data(), encoded_size);
  return payload;
}

TEST(IceoryxPayloadSize, RejectsNullPayload) {
  EXPECT_FALSE(ValidateEncodedPayloadSize(nullptr, sizeof(uint32_t)));
}

TEST(IceoryxPayloadSize, RejectsPayloadsShorterThanSizePrefix) {
  const std::array<char, 4> payload{};
  for (size_t payload_size = 0; payload_size < sizeof(uint32_t);
       ++payload_size) {
    EXPECT_FALSE(ValidateEncodedPayloadSize(payload.data(), payload_size));
  }
}

TEST(IceoryxPayloadSize, RejectsEncodedSizeShorterThanSizePrefix) {
  for (const uint32_t encoded_size : {0u, 3u}) {
    const auto payload = PayloadWithEncodedSize(encoded_size);
    EXPECT_FALSE(ValidateEncodedPayloadSize(payload.data(), payload.size()));
  }
}

TEST(IceoryxPayloadSize, RejectsEncodedSizeLargerThanPayload) {
  const auto payload = PayloadWithEncodedSize(9);
  EXPECT_FALSE(ValidateEncodedPayloadSize(payload.data(), payload.size()));
}

TEST(IceoryxPayloadSize, AcceptsExactAndContainedEncodedSizes) {
  const auto exact = PayloadWithEncodedSize(8);
  ASSERT_EQ(ValidateEncodedPayloadSize(exact.data(), exact.size()), 8u);

  const auto contained = PayloadWithEncodedSize(4);
  EXPECT_EQ(ValidateEncodedPayloadSize(contained.data(), contained.size()), 4u);
}

}  // namespace
}  // namespace ibmw::extensions::iceoryx_extension::internal
