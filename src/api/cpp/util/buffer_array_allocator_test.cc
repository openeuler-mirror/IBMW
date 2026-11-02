// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <cstdint>

#include "cpp/util/buffer_array_allocator.h"

namespace ibmw::util {

TEST(SimpleBufferArrayAllocatorTest, AllocateGrowsAndKeepsBuffers) {
  ibmw_buffer_array_t array{nullptr, 0, 0};
  for (size_t ii = 0; ii < 9; ++ii) {
    auto buf = SimpleBufferArrayAllocator::Allocate(&array, ii + 1);
    ASSERT_NE(buf.data, nullptr);
    EXPECT_EQ(buf.len, ii + 1);
  }
  EXPECT_EQ(array.len, 9u);
  EXPECT_EQ(array.capacity, 16u);
  for (size_t ii = 0; ii < array.len; ++ii) EXPECT_EQ(array.data[ii].len, ii + 1);
  SimpleBufferArrayAllocator::Release(&array);
  EXPECT_EQ(array.data, nullptr);
  EXPECT_EQ(array.len, 0u);
  EXPECT_EQ(array.capacity, 0u);
}

TEST(SimpleBufferArrayAllocatorTest, ReserveNeverShrinks) {
  ibmw_buffer_array_t array{nullptr, 0, 0};
  for (int ii = 0; ii < 3; ++ii) SimpleBufferArrayAllocator::Allocate(&array, 8);
  auto* data = array.data;
  EXPECT_TRUE(SimpleBufferArrayAllocator::Reserve(&array, 1));
  EXPECT_EQ(array.data, data);
  EXPECT_EQ(array.capacity, 4u);
  EXPECT_EQ(array.len, 3u);
  SimpleBufferArrayAllocator::Release(&array);
}

TEST(SimpleBufferArrayAllocatorTest, FailedReserveLeavesArrayUnchanged) {
  ibmw_buffer_array_t array{nullptr, 0, 0};
  SimpleBufferArrayAllocator::Allocate(&array, 8);
  const ibmw_buffer_array_t before = array;
  EXPECT_FALSE(SimpleBufferArrayAllocator::NativeHandle()->reserve(nullptr, &array, SIZE_MAX));
  EXPECT_EQ(array.data, before.data);
  EXPECT_EQ(array.len, before.len);
  EXPECT_EQ(array.capacity, before.capacity);
  SimpleBufferArrayAllocator::Release(&array);
}

TEST(SimpleBufferArrayAllocatorTest, CapacityDoublingOverflowFailsCleanly) {
  // Only the bookkeeping is faked: a full array whose doubled capacity would wrap.
  ibmw_buffer_t slot{nullptr, 0};
  ibmw_buffer_array_t array{&slot, SIZE_MAX / 2 + 1, SIZE_MAX / 2 + 1};
  auto buf = SimpleBufferArrayAllocator::Allocate(&array, 8);
  EXPECT_EQ(buf.data, nullptr);
  EXPECT_EQ(buf.len, 0u);
  EXPECT_EQ(array.data, &slot);
  EXPECT_EQ(array.len, SIZE_MAX / 2 + 1);
  EXPECT_EQ(array.capacity, SIZE_MAX / 2 + 1);
}

}  // namespace ibmw::util
