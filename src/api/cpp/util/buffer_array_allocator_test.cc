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

// API-005: buffers are carved contiguously; a size larger than what is left
// (including one that would wrap the pointer) fails without consuming bytes.
TEST(FlatBufferArrayAllocatorTest, OversizedAllocationFails) {
  char region[16];
  FlatBufferArrayAllocator allocator(region, sizeof(region));
  ibmw_buffer_array_t array{nullptr, 0, 0};
  const auto* base = allocator.NativeHandle();
  auto first = base->allocate(base->ctx, &array, 10);
  auto second = base->allocate(base->ctx, &array, 6);
  EXPECT_EQ(first.data, region);
  EXPECT_EQ(second.data, region + 10);
  EXPECT_FALSE(allocator.IsOutOfMemory());

  FlatBufferArrayAllocator wrapping(region, sizeof(region));
  ibmw_buffer_array_t other{nullptr, 0, 0};
  const auto* wrap_base = wrapping.NativeHandle();
  wrap_base->allocate(wrap_base->ctx, &other, 10);
  auto huge = wrap_base->allocate(wrap_base->ctx, &other, SIZE_MAX - 5);
  EXPECT_EQ(huge.data, nullptr);
  EXPECT_TRUE(wrapping.IsOutOfMemory());
  EXPECT_EQ(other.len, 1u);

  FlatBufferArrayAllocator::Release(&array);
  FlatBufferArrayAllocator::Release(&other);
}

// API-003: reserve below the current length must not shrink the array.
TEST(FlatBufferArrayAllocatorTest, ReserveNeverShrinks) {
  char region[64];
  FlatBufferArrayAllocator allocator(region, sizeof(region));
  ibmw_buffer_array_t array{nullptr, 0, 0};
  const auto* base = allocator.NativeHandle();
  for (int ii = 0; ii < 3; ++ii) base->allocate(base->ctx, &array, 8);
  auto* data = array.data;
  EXPECT_TRUE(base->reserve(base->ctx, &array, 1));
  EXPECT_EQ(array.data, data);
  EXPECT_EQ(array.capacity, 4u);
  EXPECT_FALSE(base->reserve(base->ctx, &array, SIZE_MAX));
  EXPECT_EQ(array.data, data);
  FlatBufferArrayAllocator::Release(&array);
}

}  // namespace ibmw::util
