// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/concurrent/concurrent_hash_map.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace concurrent_hash_map_test {

using ibmw::selfdev::concurrent::ConcurrentHashMap;

// HM-01
TEST(ConcurrentHashMapTest, EmplaceNewAndExisting) {
  ConcurrentHashMap<uint32_t, std::unique_ptr<int>> m;
  EXPECT_TRUE(m.Emplace(1, std::make_unique<int>(10)));
  auto p = std::make_unique<int>(20);
  EXPECT_FALSE(m.Emplace(1, std::move(p)));
  EXPECT_NE(p, nullptr);  // not moved from
  EXPECT_EQ(m.Size(), 1u);
}

// HM-02
TEST(ConcurrentHashMapTest, Take) {
  ConcurrentHashMap<uint32_t, std::string> m;
  ASSERT_TRUE(m.Emplace(7, "seven"));
  auto v = m.Take(7);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, "seven");
  EXPECT_FALSE(m.Contains(7));
  EXPECT_FALSE(m.Take(7).has_value());
}

// HM-03
TEST(ConcurrentHashMapTest, EraseContainsGet) {
  ConcurrentHashMap<std::string, int> m;
  ASSERT_TRUE(m.Emplace("a", 1));
  EXPECT_TRUE(m.Contains("a"));
  EXPECT_EQ(m.Get("a"), std::optional<int>(1));
  EXPECT_FALSE(m.Get("b").has_value());
  EXPECT_TRUE(m.Erase("a"));
  EXPECT_FALSE(m.Erase("a"));
  EXPECT_FALSE(m.Contains("a"));
}

// HM-04
TEST(ConcurrentHashMapTest, InsertOrAssign) {
  ConcurrentHashMap<int, int> m;
  EXPECT_TRUE(m.InsertOrAssign(1, 1));
  EXPECT_FALSE(m.InsertOrAssign(1, 2));
  EXPECT_EQ(m.Get(1), std::optional<int>(2));
}

// HM-05
TEST(ConcurrentHashMapTest, Visit) {
  ConcurrentHashMap<int, int> m;
  ASSERT_TRUE(m.Emplace(1, 1));
  EXPECT_TRUE(m.Visit(1, [](int& v) { v += 41; }));
  EXPECT_EQ(m.Get(1), std::optional<int>(42));
  bool called = false;
  EXPECT_FALSE(m.Visit(2, [&](int&) { called = true; }));
  EXPECT_FALSE(called);
  const auto& cm = m;
  int seen = 0;
  EXPECT_TRUE(cm.Visit(1, [&](const int& v) { seen = v; }));
  EXPECT_EQ(seen, 42);
}

// HM-06
TEST(ConcurrentHashMapTest, ForEachSizeClear) {
  ConcurrentHashMap<int, int> m(4);
  EXPECT_TRUE(m.Empty());
  for (int i = 0; i < 100; ++i) ASSERT_TRUE(m.Emplace(i, i * 2));
  EXPECT_EQ(m.Size(), 100u);
  int count = 0;
  long sum = 0;
  m.ForEach([&](const int& k, int& v) {
    EXPECT_EQ(v, k * 2);
    ++count;
    sum += k;
  });
  EXPECT_EQ(count, 100);
  EXPECT_EQ(sum, 99 * 100 / 2);
  m.Clear();
  EXPECT_TRUE(m.Empty());
}

// HM-07
TEST(ConcurrentHashMapTest, ShardCountRounding) {
  EXPECT_EQ((ConcurrentHashMap<int, int>(0).ShardCount()), 1u);
  EXPECT_EQ((ConcurrentHashMap<int, int>(1).ShardCount()), 1u);
  EXPECT_EQ((ConcurrentHashMap<int, int>(3).ShardCount()), 4u);
  EXPECT_EQ((ConcurrentHashMap<int, int>(16).ShardCount()), 16u);
  EXPECT_EQ((ConcurrentHashMap<int, int>().ShardCount()), 64u);
}

// HM-08
TEST(ConcurrentHashMapTest, MoveOnlyValue) {
  ConcurrentHashMap<int, std::unique_ptr<int>> m;
  ASSERT_TRUE(m.Emplace(1, std::make_unique<int>(5)));
  auto v = m.Take(1);
  ASSERT_TRUE(v.has_value());
  ASSERT_NE(*v, nullptr);
  EXPECT_EQ(**v, 5);
}

// HM-09
TEST(ConcurrentHashMapTest, ConcurrentEmplaceSameKey) {
  for (int round = 0; round < 50; ++round) {
    ConcurrentHashMap<uint32_t, int> m;
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
      threads.emplace_back([&, t] {
        if (m.Emplace(1, t)) ++winners;
      });
    for (auto& th : threads) th.join();
    ASSERT_EQ(winners.load(), 1);
  }
}

// HM-10: RPC timeout racing the response for one req_id.
TEST(ConcurrentHashMapTest, ConcurrentTakeSameKey) {
  for (int round = 0; round < 50; ++round) {
    ConcurrentHashMap<uint32_t, std::shared_ptr<int>> m;
    ASSERT_TRUE(m.Emplace(1, std::make_shared<int>(1)));
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
      threads.emplace_back([&] {
        if (auto v = m.Take(1); v && *v) ++winners;
      });
    for (auto& th : threads) th.join();
    ASSERT_EQ(winners.load(), 1);
  }
}

// HM-11
TEST(ConcurrentHashMapTest, MixedStress) {
  constexpr int kThreads = 8;
  constexpr int kOps = 50000;
  ConcurrentHashMap<uint32_t, uint64_t> m;
  std::atomic<long> inserted{0};
  std::atomic<long> taken{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<uint32_t>(t) * 7919u + 1u);
      std::uniform_int_distribution<uint32_t> key(0, 1023);
      std::uniform_int_distribution<int> op(0, 2);
      for (int i = 0; i < kOps; ++i) {
        uint32_t k = key(rng);
        switch (op(rng)) {
          case 0:
            if (m.Emplace(k, uint64_t{k})) ++inserted;
            break;
          case 1:
            if (auto v = m.Take(k)) {
              EXPECT_EQ(*v, uint64_t{k});
              ++taken;
            }
            break;
          default:
            m.Visit(k, [k](uint64_t& v) { EXPECT_EQ(v, uint64_t{k}); });
            break;
        }
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(static_cast<long>(m.Size()), inserted.load() - taken.load());
}

// HM-12: identity hashes of consecutive keys must still spread over shards.
TEST(ConcurrentHashMapTest, ShardDistribution) {
  ConcurrentHashMap<uint32_t, int> m(16);
  std::vector<int> per_shard(m.ShardCount(), 0);
  constexpr uint32_t kKeys = 65536;
  for (uint32_t k = 0; k < kKeys; ++k) ++per_shard[m.ShardIndex(k)];
  const double mean = static_cast<double>(kKeys) / static_cast<double>(m.ShardCount());
  for (size_t i = 0; i < per_shard.size(); ++i)
    EXPECT_LT(std::abs(per_shard[i] - mean), mean * 0.5) << "shard " << i << ": " << per_shard[i];
}

// HM-13: hashers that carry state and have no default constructor.
struct SeededHash {
  explicit SeededHash(uint64_t s) : seed(s) {}
  size_t operator()(uint32_t k) const { return static_cast<size_t>(k ^ seed); }
  uint64_t seed;
};

TEST(ConcurrentHashMapTest, StatefulHash) {
  ConcurrentHashMap<uint32_t, int, SeededHash> m(8, SeededHash(0x9e3779b9u));
  for (uint32_t k = 0; k < 1000; ++k) EXPECT_TRUE(m.Emplace(k, static_cast<int>(k)));
  EXPECT_EQ(m.Size(), 1000u);
  for (uint32_t k = 0; k < 1000; ++k) EXPECT_EQ(m.Get(k), std::optional<int>(static_cast<int>(k)));
  m.Clear();
  EXPECT_TRUE(m.Empty());
  EXPECT_TRUE(m.Emplace(7u, 7));
}

// HM-14: shard count rounding is clamped instead of overflowing.
TEST(ConcurrentHashMapTest, ShardCountClamped) {
  EXPECT_EQ((ConcurrentHashMap<int, int>(0).ShardCount()), 1u);
  EXPECT_EQ((ConcurrentHashMap<int, int>(5).ShardCount()), 8u);
  using Map = ConcurrentHashMap<int, int>;
  EXPECT_EQ(Map(Map::kMaxShards + 1).ShardCount(), Map::kMaxShards);
  EXPECT_EQ(Map(~size_t{0}).ShardCount(), Map::kMaxShards);
}

}  // namespace concurrent_hash_map_test
