// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Queue policies: DISCARD_OLDEST overflow and the lossless pair
// (publisher kWaitForConsumer + subscriber kBlockProducer).

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ibmw_selfdev/shm/shm.h"
#include "test_util.h"

namespace ibmw::selfdev::shm {
namespace {

using namespace std::chrono_literals;
using test::CheckPattern;
using test::FillPattern;
using test::UniqueUrl;
using test::WaitFor;

void PublishSeq(Publisher& pub, uint64_t seq, uint32_t size = 64) {
  auto l = pub.Loan(size);
  ASSERT_TRUE(l.ok()) << ErrorName(l.error());
  FillPattern(*l, size, seq);
  pub.Publish(*l);
}

PublisherOptions Lossless(std::chrono::milliseconds timeout = 0ms) {
  PublisherOptions po;
  po.too_slow = TooSlowPolicy::kWaitForConsumer;
  po.block_timeout = timeout;
  po.pools = {{64, 32}};  // segment acked during the handshake
  return po;
}

// PO-01 DISCARD_OLDEST keeps the newest `capacity` samples, in order.
TEST(ShmPolicyTest, DiscardOldestKeepsNewest) {
  auto url = UniqueUrl("po01");
  PublisherOptions po;
  po.pools = {{64, 32}};
  Publisher pub(url, po);
  Subscriber sub(url, SubscriberOptions{4, QueueFullPolicy::kDiscardOldest, 8});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  for (uint64_t s = 1; s <= 10; ++s) PublishSeq(pub, s);
  std::vector<uint64_t> got;
  while (auto t = sub.Take()) {
    got.push_back(CheckPattern(*t, 64));
    sub.Release(*t);
  }
  EXPECT_EQ(got, (std::vector<uint64_t>{7, 8, 9, 10}));
  EXPECT_EQ(pub.Stats().evictions, 6u);
  // Evicted chunks went back to the pool: no growth beyond the eager pool.
  EXPECT_EQ(pub.Stats().segments, 1u);
}

// PO-02 Lossless pair: a slow consumer receives every sample in order and
// the producer blocks instead of dropping.
TEST(ShmPolicyTest, LosslessDeliversEverything) {
  auto url = UniqueUrl("po02");
  Publisher pub(url, Lossless());
  Subscriber sub(url, SubscriberOptions{2, QueueFullPolicy::kBlockProducer, 4});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  constexpr uint64_t kCount = 2000;
  std::thread producer([&] {
    for (uint64_t s = 1; s <= kCount; ++s) PublishSeq(pub, s);
  });
  uint64_t expect = 1;
  auto deadline = std::chrono::steady_clock::now() + 30s;
  while (expect <= kCount && std::chrono::steady_clock::now() < deadline) {
    auto t = sub.Take();
    if (!t) {
      std::this_thread::yield();
      continue;
    }
    ASSERT_EQ(CheckPattern(*t, 64), expect);
    ++expect;
    sub.Release(*t);
    if (expect % 200 == 0) std::this_thread::sleep_for(1ms);  // be slow sometimes
  }
  producer.join();
  EXPECT_EQ(expect, kCount + 1);
  auto st = pub.Stats();
  EXPECT_EQ(st.evictions, 0u);
  EXPECT_GT(st.blocked, 0u);
  EXPECT_EQ(st.block_timeouts, 0u);
}

// PO-03 block_timeout bounds a blocked Publish; the sample is dropped for
// that subscriber only.
TEST(ShmPolicyTest, BlockTimeout) {
  auto url = UniqueUrl("po03");
  Publisher pub(url, Lossless(50ms));
  Subscriber sub(url, SubscriberOptions{2, QueueFullPolicy::kBlockProducer, 4});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  PublishSeq(pub, 1);
  PublishSeq(pub, 2);
  auto t0 = std::chrono::steady_clock::now();
  PublishSeq(pub, 3);
  auto dt = std::chrono::steady_clock::now() - t0;
  EXPECT_GE(dt, 45ms);
  EXPECT_LT(dt, 2s);
  EXPECT_EQ(pub.Stats().block_timeouts, 1u);
  std::vector<uint64_t> got;
  while (auto t = sub.Take()) {
    got.push_back(CheckPattern(*t, 64));
    sub.Release(*t);
  }
  EXPECT_EQ(got, (std::vector<uint64_t>{1, 2}));
}

// PO-04 Blocking needs both sides: a kBlockProducer subscriber behind a
// discarding publisher gets DISCARD_OLDEST, and vice versa.
TEST(ShmPolicyTest, BlockingRequiresBothSides) {
  auto url = UniqueUrl("po04");
  PublisherOptions po;
  po.pools = {{64, 32}};
  Publisher discarding(url, po);
  Subscriber blocking_sub(url, SubscriberOptions{2, QueueFullPolicy::kBlockProducer, 4});
  ASSERT_TRUE(WaitFor([&] { return discarding.HasSubscribers(); }));
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t s = 1; s <= 10; ++s) PublishSeq(discarding, s);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
  EXPECT_EQ(discarding.Stats().evictions, 8u);

  auto url2 = UniqueUrl("po04b");
  Publisher waiting(url2, Lossless());
  Subscriber discard_sub(url2, SubscriberOptions{2, QueueFullPolicy::kDiscardOldest, 4});
  ASSERT_TRUE(WaitFor([&] { return waiting.HasSubscribers(); }));
  for (uint64_t s = 1; s <= 10; ++s) PublishSeq(waiting, s);
  EXPECT_EQ(waiting.Stats().blocked, 0u);
  EXPECT_EQ(waiting.Stats().evictions, 8u);
}

// PO-05 A lossless subscriber does not slow down a discarding sibling, and
// a blocked Publish resumes as soon as the slow subscriber disappears.
TEST(ShmPolicyTest, BlockedPublishReleasedBySubscriberExit) {
  auto url = UniqueUrl("po05");
  Publisher pub(url, Lossless());
  auto slow = std::make_unique<Subscriber>(url, SubscriberOptions{1, QueueFullPolicy::kBlockProducer, 4});
  Subscriber fast(url, SubscriberOptions{16, QueueFullPolicy::kDiscardOldest, 4});
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 2; }));
  PublishSeq(pub, 1);  // fills `slow`
  std::atomic<bool> finished{false};
  std::thread producer([&] {
    PublishSeq(pub, 2);  // blocks on `slow`
    finished = true;
  });
  // `fast` already has sample 2 although Publish has not returned.
  ASSERT_TRUE(WaitFor([&] {
    while (auto t = fast.Take()) {
      bool two = CheckPattern(*t, 64) == 2;
      fast.Release(*t);
      if (two) return true;
    }
    return false;
  }));
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(finished.load());
  slow.reset();
  producer.join();
  EXPECT_TRUE(finished.load());
  EXPECT_EQ(pub.Stats().block_timeouts, 0u);
}

// PO-06 Several producer threads blocked on one lossless subscriber all make
// progress; nothing is lost or duplicated.
TEST(ShmPolicyTest, ManyBlockedProducers) {
  auto url = UniqueUrl("po06");
  Publisher pub(url, Lossless());
  Subscriber sub(url, SubscriberOptions{1, QueueFullPolicy::kBlockProducer, 2});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  constexpr int kThreads = 4, kPer = 500;
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&, t] {
      for (int i = 0; i < kPer; ++i) PublishSeq(pub, static_cast<uint64_t>(t) * kPer + i + 1);
    });
  }
  std::vector<int> seen(kThreads * kPer + 1, 0);
  int got = 0;
  auto deadline = std::chrono::steady_clock::now() + 30s;
  while (got < kThreads * kPer && std::chrono::steady_clock::now() < deadline) {
    auto t = sub.Take();
    if (!t) {
      std::this_thread::yield();
      continue;
    }
    uint64_t s = CheckPattern(*t, 64);
    ASSERT_LE(s, static_cast<uint64_t>(kThreads * kPer));
    ++seen[s];
    ++got;
    sub.Release(*t);
  }
  for (auto& th : ts) th.join();
  EXPECT_EQ(got, kThreads * kPer);
  for (size_t i = 1; i < seen.size(); ++i) EXPECT_EQ(seen[i], 1) << i;
}

}  // namespace
}  // namespace ibmw::selfdev::shm
