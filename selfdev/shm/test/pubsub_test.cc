// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Publisher/subscriber data path inside one process: discovery, loaning,
// size classes, alignment, fan-out, N:M, chunk recycling, held limits.

#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>

#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ibmw_selfdev/shm/shm.h"
#include "test_util.h"

namespace ibmw::selfdev::shm {
namespace {

using test::CheckPattern;
using test::FillPattern;
using test::UniqueUrl;
using test::WaitFor;

bool Connected(const Publisher& p, const Subscriber& s, uint32_t n = 1) {
  return WaitFor([&] { return p.Stats().connections >= n && s.Stats().connections >= 1; });
}

std::vector<std::string> SocketFiles() {
  std::vector<std::string> out;
  if (DIR* d = ::opendir(Runtime::ServiceDirectory().c_str())) {
    while (auto* de = ::readdir(d)) {
      std::string n = de->d_name;
      if (n.size() > 5 && n.substr(n.size() - 5) == ".sock") out.push_back(n);
    }
    ::closedir(d);
  }
  return out;
}

// PS-01 The runtime owns a private 0700 service directory under the root.
TEST(ShmPubSubTest, ServiceDirectory) {
  ASSERT_TRUE(Runtime::IsInitialized());
  std::string dir = Runtime::ServiceDirectory();
  EXPECT_EQ(dir.rfind(test::TestRoot() + "/ibmw_shm.", 0), 0u) << dir;
  struct stat st {};
  ASSERT_EQ(::stat(dir.c_str(), &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(st.st_mode & 0777, 0700u);
}

// PS-02 A publisher announces itself with one socket file, removed on destruction.
TEST(ShmPubSubTest, PublisherSocketLifecycle) {
  size_t before = SocketFiles().size();
  {
    Publisher pub(UniqueUrl("ps02"));
    ASSERT_TRUE(pub.Valid());
    EXPECT_EQ(SocketFiles().size(), before + 1);
    EXPECT_FALSE(pub.HasSubscribers());
  }
  EXPECT_EQ(SocketFiles().size(), before);
}

// PS-03 Invalid URLs are refused at construction.
TEST(ShmPubSubTest, InvalidUrl) {
  Publisher p("");
  EXPECT_FALSE(p.Valid());
  EXPECT_EQ(p.InitError(), Error::kInvalidArg);
  Subscriber s(std::string(kMaxUrlLength + 1, 'x'));
  EXPECT_FALSE(s.Valid());
  EXPECT_EQ(s.InitError(), Error::kInvalidArg);
  // Operations on invalid objects fail cleanly.
  EXPECT_EQ(p.Loan(8).error(), Error::kNotConnected);
  EXPECT_EQ(s.Take().error(), Error::kNotConnected);
  EXPECT_FALSE(s.HasData());
}

// PS-04 One message end to end; the subscriber sees the same bytes through
// its own read-only mapping, plus header metadata.
TEST(ShmPubSubTest, SingleMessage) {
  auto url = UniqueUrl("ps04");
  Publisher pub(url);
  Subscriber sub(url);
  ASSERT_TRUE(Connected(pub, sub));
  EXPECT_EQ(sub.Take().error(), Error::kNoData);

  auto loan = pub.Loan(1000);
  ASSERT_TRUE(loan.ok());
  FillPattern(*loan, 1000, 42);
  pub.Publish(*loan);

  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto t = sub.Take();
  ASSERT_TRUE(t.ok());
  EXPECT_NE(*t, *loan);  // different mapping of the same memfd
  EXPECT_EQ(CheckPattern(*t, 1000), 42u);
  auto view = ChunkView::FromUserPayload(*t);
  ASSERT_TRUE(view.Valid());
  EXPECT_EQ(view.PayloadSize(), 1000u);
  EXPECT_EQ(view.Sequence(), 1u);
  EXPECT_GT(view.PublishTimestampNs(), 0u);
  EXPECT_EQ(sub.Stats().held, 1u);
  sub.Release(*t);
  EXPECT_EQ(sub.Stats().held, 0u);
  EXPECT_FALSE(sub.HasData());

  auto ps = pub.Stats();
  EXPECT_EQ(ps.loans, 1u);
  EXPECT_EQ(ps.publishes, 1u);
  EXPECT_EQ(ps.deliveries, 1u);
  EXPECT_TRUE(ps.read_only_enforced);
}

// PS-05 At most kMaxChunksAllocatedPerPublisher simultaneous loans.
TEST(ShmPubSubTest, LoanLimit) {
  Publisher pub(UniqueUrl("ps05"));
  std::vector<void*> loans;
  for (uint32_t i = 0; i < kMaxChunksAllocatedPerPublisher; ++i) {
    auto l = pub.Loan(64);
    ASSERT_TRUE(l.ok()) << i;
    loans.push_back(*l);
  }
  EXPECT_EQ(pub.Loan(64).error(), Error::kTooManyLoans);
  pub.Release(loans.back());
  loans.pop_back();
  auto again = pub.Loan(64);
  ASSERT_TRUE(again.ok());
  loans.push_back(*again);
  // Publishing also ends a loan, even without subscribers.
  pub.Publish(loans.back());
  loans.pop_back();
  EXPECT_TRUE(pub.Loan(64).ok());
  // Releasing twice / foreign pointers are ignored.
  int local = 0;
  pub.Release(&local);
  pub.Publish(&local);
  EXPECT_EQ(pub.Stats().publishes, 1u);
}

// PS-06 Alignment up to 4096; invalid alignments refused.
TEST(ShmPubSubTest, Alignment) {
  auto url = UniqueUrl("ps06");
  Publisher pub(url);
  Subscriber sub(url, SubscriberOptions{8, QueueFullPolicy::kDiscardOldest, 8});
  ASSERT_TRUE(Connected(pub, sub));
  for (uint32_t align : {1u, 8u, 64u, 128u, 256u, 4096u}) {
    auto l = pub.Loan(100, align);
    ASSERT_TRUE(l.ok()) << align;
    EXPECT_EQ(reinterpret_cast<uintptr_t>(*l) % align, 0u) << align;
    auto v = ChunkView::FromUserPayload(*l);
    ASSERT_TRUE(v.Valid());
    EXPECT_EQ(v.PayloadAlignment(), align);
    FillPattern(*l, 100, align);
    pub.Publish(*l);
    ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
    auto t = sub.Take();
    ASSERT_TRUE(t.ok());
    EXPECT_EQ(reinterpret_cast<uintptr_t>(*t) % align, 0u) << align;
    EXPECT_EQ(CheckPattern(*t, 100), align);
    sub.Release(*t);
  }
  EXPECT_EQ(pub.Loan(8, 3).error(), Error::kInvalidArg);
  EXPECT_EQ(pub.Loan(8, 0).error(), Error::kInvalidArg);
  EXPECT_EQ(pub.Loan(8, 8192).error(), Error::kInvalidArg);
}

// PS-07 Size classes from 1 byte to 256 MB; larger is refused, and the
// shared-memory budget is enforced.
TEST(ShmPubSubTest, SizeClassesAndBudget) {
  PublisherOptions po;
  po.max_bytes = 64ull << 20;
  Publisher pub(UniqueUrl("ps07"), po);
  for (uint32_t size : {1u, 128u, 129u, 4096u, 1u << 20, (1u << 20) + 1}) {
    auto l = pub.Loan(size);
    ASSERT_TRUE(l.ok()) << size;
    std::memset(*l, 0xab, size);  // the whole payload is writable
    EXPECT_EQ(ChunkView::FromUserPayload(*l).PayloadSize(), size);
    pub.Release(*l);
  }
  EXPECT_EQ(pub.Loan((1u << kMaxClassShift) + 1).error(), Error::kInvalidArg);
  // 64 MB budget: one 32 MB chunk fits, a 64 MB one does not.
  auto big = pub.Loan(32u << 20);
  ASSERT_TRUE(big.ok());
  pub.Release(*big);
  auto too_big = pub.Loan(64u << 20);
  EXPECT_EQ(too_big.error(), Error::kNoMemory);
  EXPECT_EQ(pub.Stats().pool_exhausted, 1u);
  EXPECT_LE(pub.Stats().segment_bytes, po.max_bytes);
}

// PS-08 Eager pools exist before the first loan and are reused.
TEST(ShmPubSubTest, EagerPool) {
  PublisherOptions po;
  po.pools = {{4096, 16}};
  Publisher pub(UniqueUrl("ps08"), po);
  ASSERT_TRUE(pub.Valid());
  auto s0 = pub.Stats();
  EXPECT_GE(s0.segments, 1u);
  for (int i = 0; i < 100; ++i) {
    auto l = pub.Loan(4000);
    ASSERT_TRUE(l.ok());
    pub.Publish(*l);
  }
  EXPECT_EQ(pub.Stats().segments, s0.segments);
}

// PS-09 1:N fan-out: every subscriber gets every message, zero copy.
TEST(ShmPubSubTest, FanOut) {
  auto url = UniqueUrl("ps09");
  Publisher pub(url);
  SubscriberOptions so{64, QueueFullPolicy::kDiscardOldest, 64};
  std::vector<std::unique_ptr<Subscriber>> subs;
  for (int i = 0; i < 4; ++i) subs.push_back(std::make_unique<Subscriber>(url, so));
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 4; }));
  for (uint64_t seq = 1; seq <= 32; ++seq) {
    auto l = pub.Loan(256);
    ASSERT_TRUE(l.ok());
    FillPattern(*l, 256, seq);
    pub.Publish(*l);
  }
  for (auto& s : subs) {
    uint64_t expect = 1;
    ASSERT_TRUE(WaitFor([&] {
      while (auto t = s->Take()) {
        EXPECT_EQ(CheckPattern(*t, 256), expect);
        ++expect;
        s->Release(*t);
      }
      return expect == 33;
    }));
  }
  EXPECT_EQ(pub.Stats().deliveries, 32u * 4);
}

// PS-10 N:M: each subscriber receives from every publisher of the URL.
TEST(ShmPubSubTest, ManyToMany) {
  auto url = UniqueUrl("ps10");
  SubscriberOptions so{64, QueueFullPolicy::kDiscardOldest, 64};
  Subscriber s1(url, so);
  Publisher p1(url);
  Publisher p2(url);
  Subscriber s2(url, so);
  ASSERT_TRUE(WaitFor([&] {
    return p1.Stats().connections == 2 && p2.Stats().connections == 2 && s1.Stats().connections == 2 &&
           s2.Stats().connections == 2;
  }));
  for (uint64_t i = 0; i < 10; ++i) {
    for (auto* p : {&p1, &p2}) {
      auto l = p->Loan(64);
      ASSERT_TRUE(l.ok());
      FillPattern(*l, 64, (p == &p1 ? 1000 : 2000) + i);
      p->Publish(*l);
    }
  }
  for (auto* s : {&s1, &s2}) {
    std::set<uint64_t> got;
    ASSERT_TRUE(WaitFor([&] {
      while (auto t = s->Take()) {
        got.insert(CheckPattern(*t, 64));
        s->Release(*t);
      }
      return got.size() == 20;
    }));
    EXPECT_EQ(*got.begin(), 1000u);
    EXPECT_EQ(*got.rbegin(), 2009u);
  }
}

// PS-11 Discovery works in both orders, and URLs do not cross-talk.
TEST(ShmPubSubTest, DiscoveryOrderAndIsolation) {
  auto url = UniqueUrl("ps11");
  auto other = UniqueUrl("ps11other");
  Subscriber early(url);
  Subscriber unrelated(other);
  Publisher pub(url);
  Subscriber late(url);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 2; }));
  EXPECT_EQ(early.Stats().connections, 1u);
  EXPECT_EQ(late.Stats().connections, 1u);
  EXPECT_EQ(unrelated.Stats().connections, 0u);
}

// PS-12 Steady-state publishing recycles chunks: no pool growth, no leaks.
TEST(ShmPubSubTest, ChunkRecycling) {
  auto url = UniqueUrl("ps12");
  Publisher pub(url);
  Subscriber sub(url, SubscriberOptions{4, QueueFullPolicy::kDiscardOldest, 4});
  ASSERT_TRUE(Connected(pub, sub));
  constexpr int kMessages = 20000;
  int received = 0;
  for (int i = 0; i < kMessages; ++i) {
    auto l = pub.Loan(512);
    ASSERT_TRUE(l.ok()) << i << " " << ErrorName(l.error());
    FillPattern(*l, 512, i);
    pub.Publish(*l);
    while (auto t = sub.Take()) {
      EXPECT_NE(CheckPattern(*t, 512), ~0ull);
      sub.Release(*t);
      ++received;
    }
  }
  // Chunks parked for a segment ack are flushed by the control thread.
  WaitFor([&] {
    while (auto t = sub.Take()) {
      sub.Release(*t);
      ++received;
    }
    auto s = pub.Stats();
    return static_cast<uint64_t>(received) + s.evictions == s.deliveries;
  });
  auto st = pub.Stats();
  // Queue (4) + held (4) + loans (8) bounds the live chunks; growth roughly
  // doubles, so a handful of segments at most.
  EXPECT_LE(st.segments, 4u);
  EXPECT_EQ(st.bad_returns, 0u);
  EXPECT_GT(received, kMessages / 2);
  EXPECT_EQ(static_cast<uint64_t>(received) + st.evictions, st.deliveries);
}

// PS-13 max_held: Take refuses beyond the limit until something is released.
TEST(ShmPubSubTest, HeldLimit) {
  auto url = UniqueUrl("ps13");
  Publisher pub(url);
  Subscriber sub(url, SubscriberOptions{8, QueueFullPolicy::kDiscardOldest, 3});
  ASSERT_TRUE(Connected(pub, sub));
  for (int i = 0; i < 5; ++i) {
    auto l = pub.Loan(64);
    ASSERT_TRUE(l.ok());
    pub.Publish(*l);
  }
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  std::vector<const void*> held;
  ASSERT_TRUE(WaitFor([&] {
    while (held.size() < 3) {
      auto t = sub.Take();
      if (!t) return false;
      held.push_back(*t);
    }
    return true;
  }));
  EXPECT_EQ(sub.Take().error(), Error::kTooManyHeld);
  sub.Release(held.back());
  held.pop_back();
  EXPECT_TRUE(sub.Take().ok());
  // Unknown pointers are ignored by Release.
  int local = 0;
  sub.Release(&local);
  EXPECT_EQ(sub.Stats().held, 3u);
}

// PS-14 Chunks taken before the publisher goes away stay readable until
// released; the connection is dropped afterwards.
TEST(ShmPubSubTest, HeldChunkOutlivesPublisher) {
  auto url = UniqueUrl("ps14");
  Subscriber sub(url);
  const void* held = nullptr;
  {
    Publisher pub(url);
    ASSERT_TRUE(Connected(pub, sub));
    auto l = pub.Loan(128);
    ASSERT_TRUE(l.ok());
    FillPattern(*l, 128, 9);
    pub.Publish(*l);
    ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
    auto t = sub.Take();
    ASSERT_TRUE(t.ok());
    held = *t;
  }
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 0; }));
  EXPECT_EQ(CheckPattern(held, 128), 9u);
  sub.Release(held);
  EXPECT_EQ(sub.Stats().held, 0u);
}

// PS-15 A new segment created after the connection is announced before use
// (segment ack gating): large messages never arrive unmapped.
TEST(ShmPubSubTest, LateSegmentsAreAnnounced) {
  auto url = UniqueUrl("ps15");
  Publisher pub(url);
  Subscriber sub(url, SubscriberOptions{16, QueueFullPolicy::kDiscardOldest, 16});
  ASSERT_TRUE(Connected(pub, sub));
  // Every size class is new: each publish creates a segment first.
  std::vector<uint32_t> sizes = {100, 1000, 10000, 100000, 1000000, 4000000};
  for (size_t i = 0; i < sizes.size(); ++i) {
    auto l = pub.Loan(sizes[i]);
    ASSERT_TRUE(l.ok());
    FillPattern(*l, sizes[i], i);
    pub.Publish(*l);
  }
  size_t got = 0;
  ASSERT_TRUE(WaitFor([&] {
    while (auto t = sub.Take()) {
      uint32_t size = ChunkView::FromUserPayload(*t).PayloadSize();
      EXPECT_EQ(size, sizes[got]);
      EXPECT_EQ(CheckPattern(*t, size), got);
      sub.Release(*t);
      ++got;
    }
    return got == sizes.size();
  }));
}

// PS-16 Concurrent publishers/subscribers in threads (TSAN target):
// 4 producer threads x 25k loans on one publisher, 2 subscriber threads.
TEST(ShmPubSubTest, ConcurrentThreads) {
  auto url = UniqueUrl("ps16");
  Publisher pub(url);
  SubscriberOptions so{64, QueueFullPolicy::kDiscardOldest, 16};
  Subscriber s1(url, so), s2(url, so);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 2; }));
  constexpr int kThreads = 4, kPerThread = 25000;
  std::atomic<bool> done{false};
  std::atomic<uint64_t> taken{0}, corrupt{0};
  auto consume = [&](Subscriber& s) {
    while (!done.load() || s.HasData()) {
      auto t = s.Take();
      if (!t) {
        std::this_thread::yield();
        continue;
      }
      if (CheckPattern(*t, 96) == ~0ull) corrupt.fetch_add(1);
      s.Release(*t);
      taken.fetch_add(1);
    }
  };
  std::thread c1(consume, std::ref(s1)), c2(consume, std::ref(s2));
  std::vector<std::thread> producers;
  std::atomic<uint64_t> published{0};
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        auto l = pub.Loan(96);
        if (!l) {
          std::this_thread::yield();
          --i;
          continue;
        }
        FillPattern(*l, 96, static_cast<uint64_t>(t) * kPerThread + i);
        pub.Publish(*l);
        published.fetch_add(1);
      }
    });
  }
  for (auto& p : producers) p.join();
  done = true;
  c1.join();
  c2.join();
  auto st = pub.Stats();
  EXPECT_EQ(published.load(), static_cast<uint64_t>(kThreads) * kPerThread);
  EXPECT_EQ(st.publishes, published.load());
  EXPECT_EQ(corrupt.load(), 0u);
  EXPECT_EQ(st.bad_returns, 0u);
  EXPECT_EQ(taken.load() + st.evictions, st.deliveries);
}

}  // namespace
}  // namespace ibmw::selfdev::shm
