// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <vector>

#include "cpp/util/buffer.h"

namespace ibmw::util {
namespace {

// API-029: a BufferArray built from an empty allocator ref can be destroyed.
TEST(BufferArrayTest, EmptyAllocatorRefIsDestructible) {
  BufferArray array{BufferArrayAllocatorRef()};
  EXPECT_EQ(array.Size(), 0u);
}

// API-029: a failed reserve is reported to the caller.
TEST(BufferArrayTest, ReserveReportsFailure) {
  ibmw_buffer_array_allocator_t failing{
      .reserve = [](void*, ibmw_buffer_array_t*, size_t) { return false; },
      .allocate = [](void*, ibmw_buffer_array_t*, size_t) { return ibmw_buffer_t{}; },
      .release = [](void*, ibmw_buffer_array_t*) {},
      .ctx = nullptr};
  BufferArray array(&failing);
  EXPECT_FALSE(array.Reserve(16));
  EXPECT_FALSE(BufferArrayAllocatorRef(&failing).Reserve(array.BufferArrayNativeHandle(), 16));

  BufferArray simple;
  EXPECT_TRUE(simple.Reserve(16));
  EXPECT_GE(simple.Capacity(), 16u);
}

TEST(BufferArrayViewTest, JoinSkipsEmptyBuffers) {
  const char data[] = "abc";
  const BufferArrayView view(std::vector<ibmw_buffer_view_t>{
      {.data = nullptr, .len = 0}, {.data = data, .len = 3}, {.data = nullptr, .len = 0}});
  EXPECT_EQ(view.JoinToCharVector(), (std::vector<char>{'a', 'b', 'c'}));
  EXPECT_EQ(view.JoinToString(), "abc");
}

}  // namespace
}  // namespace ibmw::util
