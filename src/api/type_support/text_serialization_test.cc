// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Hostile JSON text messages must be rejected with an exception that the
// type-support C callbacks turn into `false`, never with a crash.

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

#include "cpp/util/buffer_array_allocator.h"
#include "type_support/text_serialization.h"

namespace ibmw::util::text {
namespace {

ibmw_buffer_array_view_t View(const std::string& text, ibmw_buffer_view_t& slot) {
  slot = ibmw_buffer_view_t{.data = text.data(), .len = text.size()};
  return ibmw_buffer_array_view_t{.data = &slot, .len = 1};
}

TEST(TextSerialization, DeeplyNestedInputIsRejectedWithoutStackOverflow) {
  constexpr size_t kDepth = 1000000;
  const std::string text = std::string(kDepth, '[') + std::string(kDepth, ']');
  ibmw_buffer_view_t slot;
  std::vector<int32_t> out;
  EXPECT_THROW(Read(Parse(View(text, slot)), out), std::invalid_argument);
}

TEST(TextSerialization, MalformedInputThrowsInsteadOfCrashing) {
  ibmw_buffer_view_t slot;
  const std::string text = "{\"a\":";
  EXPECT_THROW(Parse(View(text, slot)), std::exception);
  EXPECT_THROW(Parse(ibmw_buffer_array_view_t{nullptr, 0}), std::invalid_argument);
}

TEST(TextSerialization, StoreRejectsNonFiniteAndMissingAllocator) {
  ibmw_buffer_array_t buffers{nullptr, 0, 0};
  const auto* allocator = SimpleBufferArrayAllocator::NativeHandle();
  EXPECT_FALSE(Store(Json(1), nullptr, &buffers));
  EXPECT_THROW(Store(Json::array({1.0, NAN}), allocator, &buffers), std::invalid_argument);
  EXPECT_EQ(buffers.len, 0u);
  ASSERT_TRUE(Store(Json::array({1, 2}), allocator, &buffers));
  ASSERT_EQ(buffers.len, 1u);
  EXPECT_EQ(std::string(static_cast<const char*>(buffers.data[0].data), buffers.data[0].len), "[1,2]");
  SimpleBufferArrayAllocator::Release(&buffers);
}

}  // namespace
}  // namespace ibmw::util::text
