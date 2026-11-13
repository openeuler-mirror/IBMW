// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Node reuse in ConcurrentHashMap, seen through the global operator new:
// its own executable, which wraps operator new (ld --wrap, as in
// bounded_blocking_queue_alloc_test.cc) to count and fail allocations on
// this thread.

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>

#include <gtest/gtest.h>

#include "ibmw_selfdev/concurrent/concurrent_hash_map.h"

namespace {

thread_local bool fail_all = false;
thread_local uint64_t allocations = 0;

}  // namespace

// Calls of operator new(std::size_t) from this executable's own code (the
// map's node allocations among them) land here; see CMakeLists.txt.
extern "C" void* __real__Znwm(std::size_t n);

extern "C" void* __wrap__Znwm(std::size_t n) {
  if (fail_all) throw std::bad_alloc();
  ++allocations;
  return __real__Znwm(n);
}

namespace concurrent_hash_map_alloc_test {

using ibmw::selfdev::concurrent::ConcurrentHashMap;

struct FailScope {
  FailScope() { fail_all = true; }
  ~FailScope() { fail_all = false; }
  FailScope(const FailScope&) = delete;
  FailScope& operator=(const FailScope&) = delete;
};

// HM-25: Emplace + Take churn (RpcClientTool's Record / GetRecord) reuses
// the node freed by Take: no allocation once the shard has buckets and a
// cached node.
TEST(ConcurrentHashMapAllocTest, ChurnDoesNotAllocate) {
  ConcurrentHashMap<uint32_t, uint64_t> m(1);
  ASSERT_TRUE(m.Emplace(0, 0));
  ASSERT_TRUE(m.Take(0).has_value());
  const uint64_t before = allocations;
  for (uint32_t k = 1; k <= 1000; ++k) {
    ASSERT_TRUE(m.Emplace(k, k));
    ASSERT_EQ(m.Take(k).value_or(0), k);
  }
  EXPECT_EQ(allocations, before);
}

// HM-26: when operator new fails, Emplace still succeeds from cached nodes,
// then throws std::bad_alloc and leaves the map as it was.
TEST(ConcurrentHashMapAllocTest, AllocationFailureAfterCacheRunsOut) {
  ConcurrentHashMap<uint32_t, uint64_t> m(1);
  for (uint32_t k = 0; k < 8; ++k) ASSERT_TRUE(m.Emplace(k, k));  // buckets for 8+
  ASSERT_TRUE(m.Erase(0));
  ASSERT_TRUE(m.Erase(1));  // two cached nodes
  // Checked after the scope: gtest allocates to report a failure.
  bool first = false;
  bool second = false;
  bool threw = false;
  {
    FailScope fail;
    first = m.Emplace(100, 100);
    second = m.Emplace(101, 101);
    try {
      m.Emplace(102, 102);
    } catch (const std::bad_alloc&) {
      threw = true;
    }
  }
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_TRUE(threw);
  EXPECT_FALSE(m.Contains(102));
  EXPECT_EQ(m.Size(), 8u);
  EXPECT_EQ(m.Get(101).value_or(0), 101u);
  EXPECT_TRUE(m.Emplace(102, 102));
}

}  // namespace concurrent_hash_map_alloc_test
