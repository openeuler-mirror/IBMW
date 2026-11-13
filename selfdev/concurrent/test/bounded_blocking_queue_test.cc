// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ibmw_selfdev/concurrent/bounded_blocking_queue.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <ratio>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace bounded_blocking_queue_test {

using ibmw::selfdev::concurrent::BoundedBlockingQueue;
using namespace std::chrono_literals;

// Long enough that a thread which is not blocked has certainly returned.
constexpr auto kSettle = 50ms;

// BQ-01
TEST(BoundedBlockingQueueTest, FifoOrder) {
  BoundedBlockingQueue<int> q;
  for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.Push(i));
  EXPECT_EQ(q.Size(), 5u);
  for (int i = 0; i < 5; ++i) {
    int v = -1;
    ASSERT_TRUE(q.Pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_TRUE(q.Empty());
}

// BQ-02
TEST(BoundedBlockingQueueTest, TryPopOnEmpty) {
  BoundedBlockingQueue<int> q;
  int v = 7;
  EXPECT_FALSE(q.TryPop(v));
  EXPECT_EQ(v, 7);
}

// BQ-03
TEST(BoundedBlockingQueueTest, TryPushRespectsCapacity) {
  BoundedBlockingQueue<int> q(2);
  EXPECT_TRUE(q.TryPush(1));
  EXPECT_TRUE(q.TryEmplace(2));
  EXPECT_FALSE(q.TryPush(3));
  EXPECT_EQ(q.Size(), 2u);
}

// BQ-04
TEST(BoundedBlockingQueueTest, ZeroCapacityMeansOne) {
  BoundedBlockingQueue<int> q(0);
  EXPECT_EQ(q.Capacity(), 1u);
  EXPECT_TRUE(q.TryPush(1));
  EXPECT_FALSE(q.TryPush(2));
  q.SetCapacity(0);
  EXPECT_EQ(q.Capacity(), 1u);
  EXPECT_EQ(BoundedBlockingQueue<int>().Capacity(), BoundedBlockingQueue<int>::kUnbounded);
}

// BQ-05
TEST(BoundedBlockingQueueTest, PushBlocksWhileFull) {
  BoundedBlockingQueue<int> q(1);
  ASSERT_TRUE(q.Push(1));
  std::atomic<bool> done{false};
  std::thread producer([&] {
    EXPECT_TRUE(q.Push(2));
    done = true;
  });
  std::this_thread::sleep_for(kSettle);
  EXPECT_FALSE(done.load());
  int v = 0;
  ASSERT_TRUE(q.Pop(v));
  EXPECT_EQ(v, 1);
  producer.join();
  EXPECT_TRUE(done.load());
  ASSERT_TRUE(q.Pop(v));
  EXPECT_EQ(v, 2);
}

// BQ-06
TEST(BoundedBlockingQueueTest, PopBlocksWhileEmpty) {
  BoundedBlockingQueue<int> q;
  std::atomic<int> got{-1};
  std::thread consumer([&] {
    int v = 0;
    EXPECT_TRUE(q.Pop(v));
    got = v;
  });
  std::this_thread::sleep_for(kSettle);
  EXPECT_EQ(got.load(), -1);
  ASSERT_TRUE(q.Push(42));
  consumer.join();
  EXPECT_EQ(got.load(), 42);
}

// BQ-07
TEST(BoundedBlockingQueueTest, AbortWakesBlockedPop) {
  BoundedBlockingQueue<int> q;
  std::atomic<bool> result{true};
  std::thread consumer([&] {
    int v = 0;
    result = q.Pop(v);
  });
  std::this_thread::sleep_for(kSettle);
  auto start = std::chrono::steady_clock::now();
  q.Abort();
  consumer.join();
  EXPECT_FALSE(result.load());
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
}

// BQ-08
TEST(BoundedBlockingQueueTest, AbortWakesBlockedPush) {
  BoundedBlockingQueue<int> q(1);
  ASSERT_TRUE(q.Push(1));
  std::atomic<bool> result{true};
  std::thread producer([&] { result = q.Push(2); });
  std::this_thread::sleep_for(kSettle);
  q.Abort();
  producer.join();
  EXPECT_FALSE(result.load());
  EXPECT_EQ(q.Size(), 1u);
}

// BQ-09
TEST(BoundedBlockingQueueTest, AbortIsSticky) {
  BoundedBlockingQueue<int> q;
  ASSERT_TRUE(q.Push(1));
  ASSERT_TRUE(q.Push(2));
  q.Abort();
  EXPECT_TRUE(q.Aborted());
  int v = 0;
  EXPECT_FALSE(q.Pop(v));
  EXPECT_FALSE(q.PopFor(v, 1s));
  EXPECT_FALSE(q.Push(3));
  EXPECT_FALSE(q.TryPush(3));
  EXPECT_FALSE(q.Emplace(3));
  // Remaining items can still be drained.
  ASSERT_TRUE(q.TryPop(v));
  EXPECT_EQ(v, 1);
  ASSERT_TRUE(q.TryPop(v));
  EXPECT_EQ(v, 2);
  EXPECT_FALSE(q.TryPop(v));
}

// BQ-10
TEST(BoundedBlockingQueueTest, ResetClearsAbort) {
  BoundedBlockingQueue<int> q;
  q.Abort();
  q.Reset();
  EXPECT_FALSE(q.Aborted());
  EXPECT_TRUE(q.Push(5));
  int v = 0;
  EXPECT_TRUE(q.Pop(v));
  EXPECT_EQ(v, 5);
}

// BQ-11
TEST(BoundedBlockingQueueTest, MoveOnlyType) {
  BoundedBlockingQueue<std::unique_ptr<int>> q;
  ASSERT_TRUE(q.Push(std::make_unique<int>(3)));
  ASSERT_TRUE(q.Emplace(new int(4)));
  std::unique_ptr<int> out;
  ASSERT_TRUE(q.Pop(out));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(*out, 3);
  ASSERT_TRUE(q.TryPop(out));
  EXPECT_EQ(*out, 4);
}

// BQ-12
TEST(BoundedBlockingQueueTest, FailedPushDoesNotMove) {
  BoundedBlockingQueue<std::unique_ptr<int>> q(1);
  auto p = std::make_unique<int>(1);
  ASSERT_TRUE(q.TryPush(std::make_unique<int>(0)));
  EXPECT_FALSE(q.TryPush(std::move(p)));  // full
  EXPECT_NE(p, nullptr);
  q.Abort();
  EXPECT_FALSE(q.Push(std::move(p)));  // aborted
  EXPECT_NE(p, nullptr);
}

// BQ-13
TEST(BoundedBlockingQueueTest, PopForTimesOut) {
  BoundedBlockingQueue<int> q;
  int v = 9;
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(q.PopFor(v, 20ms));
  EXPECT_GE(std::chrono::steady_clock::now() - start, 20ms);
  EXPECT_EQ(v, 9);
  ASSERT_TRUE(q.Push(1));
  EXPECT_TRUE(q.PopFor(v, 20ms));
  EXPECT_EQ(v, 1);
}

// BQ-14
TEST(BoundedBlockingQueueTest, GrowingCapacityWakesProducer) {
  BoundedBlockingQueue<int> q(1);
  ASSERT_TRUE(q.Push(1));
  std::atomic<bool> done{false};
  std::thread producer([&] {
    EXPECT_TRUE(q.Push(2));
    done = true;
  });
  std::this_thread::sleep_for(kSettle);
  EXPECT_FALSE(done.load());
  q.SetCapacity(2);
  producer.join();
  EXPECT_EQ(q.Size(), 2u);
}

// BQ-15
TEST(BoundedBlockingQueueTest, ClearWakesProducer) {
  BoundedBlockingQueue<int> q(1);
  ASSERT_TRUE(q.Push(1));
  std::thread producer([&] { EXPECT_TRUE(q.Push(2)); });
  std::this_thread::sleep_for(kSettle);
  q.Clear();
  producer.join();
  EXPECT_EQ(q.Size(), 1u);
  int v = 0;
  ASSERT_TRUE(q.TryPop(v));
  EXPECT_EQ(v, 2);
}

// BQ-16
TEST(BoundedBlockingQueueTest, MpmcStress) {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 100000;
  BoundedBlockingQueue<int64_t> q(64);
  std::atomic<int64_t> sum{0};
  std::atomic<int64_t> count{0};
  std::vector<std::atomic<uint8_t>> seen(kProducers * kPerProducer);

  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      int64_t v;
      while (q.Pop(v)) {
        seen[static_cast<size_t>(v)].fetch_add(1);
        sum += v;
        ++count;
      }
      while (q.TryPop(v)) {
        seen[static_cast<size_t>(v)].fetch_add(1);
        sum += v;
        ++count;
      }
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) ASSERT_TRUE(q.Push(int64_t{p} * kPerProducer + i));
    });
  }
  for (auto& t : producers) t.join();
  while (!q.Empty()) std::this_thread::yield();
  q.Abort();
  for (auto& t : consumers) t.join();

  const int64_t n = int64_t{kProducers} * kPerProducer;
  EXPECT_EQ(count.load(), n);
  EXPECT_EQ(sum.load(), n * (n - 1) / 2);
  for (size_t i = 0; i < seen.size(); ++i) ASSERT_EQ(seen[i].load(), 1) << i;
}

// BQ-17: ParallelExecutor shutdown sequence.
TEST(BoundedBlockingQueueTest, ShutdownWakesAllWorkers) {
  BoundedBlockingQueue<int> q;
  std::atomic<int> exited{0};
  std::vector<std::thread> workers;
  for (int i = 0; i < 8; ++i) {
    workers.emplace_back([&] {
      int v;
      while (q.Pop(v)) {
      }
      ++exited;
    });
  }
  std::this_thread::sleep_for(kSettle);
  q.Abort();
  for (auto& t : workers) t.join();
  EXPECT_EQ(exited.load(), 8);
}

// BQ-18
struct ThrowOnMove {
  static inline bool armed = false;
  int v = 0;
  explicit ThrowOnMove(int x) : v(x) {}
  ThrowOnMove(const ThrowOnMove&) = default;
  ThrowOnMove(ThrowOnMove&& o) : v(o.v) {
    if (armed) throw std::runtime_error("move");
  }
  ThrowOnMove& operator=(ThrowOnMove&& o) {
    if (armed) throw std::runtime_error("move");
    v = o.v;
    return *this;
  }
};

TEST(BoundedBlockingQueueTest, ThrowingMoveLeavesQueueUsable) {
  BoundedBlockingQueue<ThrowOnMove> q;
  ASSERT_TRUE(q.Emplace(1));
  ThrowOnMove::armed = true;
  EXPECT_THROW(q.Push(ThrowOnMove(2)), std::runtime_error);
  EXPECT_EQ(q.Size(), 1u);
  ThrowOnMove out(0);
  EXPECT_THROW(q.Pop(out), std::runtime_error);
  EXPECT_EQ(q.Size(), 1u);  // item stays when the move into `out` throws
  ThrowOnMove::armed = false;
  ASSERT_TRUE(q.Pop(out));
  EXPECT_EQ(out.v, 1);
  EXPECT_TRUE(q.Push(ThrowOnMove(3)));
}

// BQ-19: items are spread over 8 lanes of 16-cell blocks (a lane moves to a
// new block every 128 items); Clear() at any position must leave a queue
// that keeps FIFO order across later blocks.
TEST(BoundedBlockingQueueTest, ClearAtAnyBlockOffset) {
  for (int pushed = 0; pushed < 300; ++pushed) {
    BoundedBlockingQueue<std::unique_ptr<int>> q;
    for (int i = 0; i < pushed; ++i) ASSERT_TRUE(q.Push(std::make_unique<int>(i)));
    std::unique_ptr<int> out;
    for (int i = 0; i < pushed / 2; ++i) ASSERT_TRUE(q.TryPop(out));
    q.Clear();
    EXPECT_EQ(q.Size(), 0u);
    EXPECT_FALSE(q.TryPop(out));
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(q.Push(std::make_unique<int>(i)));
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(q.TryPop(out));
      EXPECT_EQ(*out, i) << "pushed before Clear: " << pushed;
    }
    EXPECT_TRUE(q.Empty());
  }
}

// BQ-20: a constructor that throws, for any position (block ends included),
// must leave the queue consistent: the failed item is neither counted nor
// popped.
struct ThrowOnValue {
  explicit ThrowOnValue(int value, int fail_at) : v(value) {
    if (value == fail_at) throw std::runtime_error("ctor");
  }
  int v;
};

TEST(BoundedBlockingQueueTest, ThrowingEmplaceAtBlockEnd) {
  for (int fail_at = 0; fail_at < 280; ++fail_at) {
    BoundedBlockingQueue<ThrowOnValue> q;
    size_t pushed = 0;
    for (int i = 0; i < 300; ++i) {
      try {
        if (q.Emplace(i, fail_at)) ++pushed;
      } catch (const std::runtime_error&) {
        EXPECT_EQ(i, fail_at);
      }
    }
    EXPECT_EQ(q.Size(), pushed);
    ThrowOnValue out(-1, -2);
    int expected = 0;
    while (q.TryPop(out)) {
      if (expected == fail_at) ++expected;
      EXPECT_EQ(out.v, expected++);
    }
    EXPECT_EQ(expected, 300);
  }
}

// BQ-21: with more producers than lanes, each producer's items still come
// out in the order it pushed them, bounded or not.
TEST(BoundedBlockingQueueTest, PerProducerFifoWithManyProducers) {
  constexpr int kProducers = 12;
  constexpr int kPerProducer = 20000;
  for (size_t capacity : {BoundedBlockingQueue<int64_t>::kUnbounded, size_t{100}}) {
    BoundedBlockingQueue<int64_t> q(capacity);
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
      producers.emplace_back([&, p] {
        for (int i = 0; i < kPerProducer; ++i) ASSERT_TRUE(q.Push(int64_t{p} * kPerProducer + i));
      });
    }
    std::vector<int64_t> next(kProducers, 0);
    int64_t v;
    for (int n = 0; n < kProducers * kPerProducer; ++n) {
      ASSERT_TRUE(q.Pop(v));
      const auto p = static_cast<size_t>(v / kPerProducer);
      ASSERT_EQ(v % kPerProducer, next[p]) << "producer " << p;
      ++next[p];
    }
    for (auto& t : producers) t.join();
    EXPECT_TRUE(q.Empty());
  }
}

// ---- Review fixes (BQ-22..BQ-34; BQ-24..BQ-26 are in
// bounded_blocking_queue_alloc_test.cc, which replaces operator new).

// Polls `done` until it holds or `limit` passes, so a lost wakeup fails the
// test instead of hanging it.
template <class Done>
bool Eventually(Done done, std::chrono::milliseconds limit = 10s) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

double ThreadCpuMs() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) / 1e6;
}

int64_t ElapsedMs(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

// Owns heap state (ASAN reports a double destroy or a leak) and counts live
// objects. Its constructor can be made to throw. A nothrow move selects the
// lock-free consumer path, a throwing one the locked path.
template <bool kNothrowMove>
struct Item {
  static inline std::atomic<long> live{0};
  std::unique_ptr<int64_t> p;
  Item() { ++live; }
  Item(int64_t x, bool fail) : p(std::make_unique<int64_t>(x)) {
    if (fail) throw std::runtime_error("ctor");
    ++live;
  }
  Item(Item&& o) noexcept(kNothrowMove) : p(std::move(o.p)) { ++live; }
  Item& operator=(Item&& o) noexcept(kNothrowMove) {
    p = std::move(o.p);
    return *this;
  }
  ~Item() { --live; }
  int64_t value() const { return p ? *p : -1; }
};

using TicketedItem = Item<true>;
using LockedItem = Item<false>;
static_assert(std::is_nothrow_move_constructible_v<TicketedItem>);
static_assert(!std::is_nothrow_move_constructible_v<LockedItem>);

// `producers` threads push `per_producer` items each (alternately Emplace
// and TryEmplace); the constructor of item i of producer p throws when
// (i + p) % fail_every == 0. `consumers` threads mix Pop, TryPop and PopFor.
// Every item that did not throw must come out exactly once, and every
// consumer must see each producer's items in push order. `during` runs on
// its own thread until the producers are done.
template <class T>
void RunMpmc(BoundedBlockingQueue<T>& q, int producers, int consumers, int per_producer, int fail_every,
             const std::function<void(const std::atomic<bool>& stop)>& during = nullptr) {
  const auto fails = [&](int p, int i) { return fail_every != 0 && (i + p) % fail_every == 0; };
  int64_t expected = 0;
  for (int p = 0; p < producers; ++p) {
    for (int i = 0; i < per_producer; ++i) expected += fails(p, i) ? 0 : 1;
  }
  std::vector<std::atomic<int>> seen(static_cast<size_t>(producers) * static_cast<size_t>(per_producer));
  std::atomic<int64_t> got{0};
  std::atomic<int> order_errors{0};
  std::atomic<int> throws{0};

  std::vector<std::thread> threads;
  for (int c = 0; c < consumers; ++c) {
    threads.emplace_back([&, c] {
      std::mt19937 rng(static_cast<unsigned>(c));
      std::vector<int64_t> last(static_cast<size_t>(producers), -1);
      T out;
      const auto record = [&] {
        const int64_t v = out.value();
        const auto p = static_cast<size_t>(v / per_producer);
        if (last[p] >= v % per_producer) ++order_errors;
        last[p] = v % per_producer;
        seen[static_cast<size_t>(v)].fetch_add(1);
        ++got;
      };
      while (true) {
        bool ok = false;
        switch (rng() % 3) {
          case 0:
            ok = q.Pop(out);
            break;
          case 1:
            ok = q.TryPop(out);
            break;
          default:
            ok = q.PopFor(out, std::chrono::microseconds(rng() % 200));
            break;
        }
        if (ok) {
          record();
        } else if (q.Aborted()) {
          break;
        }
      }
      while (q.TryPop(out)) record();
    });
  }
  std::vector<std::thread> pushers;
  for (int p = 0; p < producers; ++p) {
    pushers.emplace_back([&, p] {
      for (int i = 0; i < per_producer; ++i) {
        const int64_t v = int64_t{p} * per_producer + i;
        try {
          if (i % 2 == 1) {
            EXPECT_TRUE(q.Emplace(v, fails(p, i)));
          } else {
            while (!q.TryEmplace(v, fails(p, i))) std::this_thread::yield();
          }
          EXPECT_FALSE(fails(p, i));
        } catch (const std::runtime_error&) {
          EXPECT_TRUE(fails(p, i));
          ++throws;
        }
      }
    });
  }
  std::atomic<bool> stop{false};
  std::thread side;
  if (during) side = std::thread([&] { during(stop); });
  for (auto& t : pushers) t.join();
  stop = true;
  if (side.joinable()) side.join();
  EXPECT_TRUE(Eventually([&] { return got.load() >= expected; }, 60s)) << "got " << got << " of " << expected;
  q.Abort();
  for (auto& t : threads) t.join();

  EXPECT_EQ(got.load(), expected);
  EXPECT_EQ(order_errors.load(), 0);
  EXPECT_EQ(int64_t{throws.load()} + expected, int64_t{producers} * per_producer);
  for (int p = 0; p < producers; ++p) {
    for (int i = 0; i < per_producer; ++i) {
      ASSERT_EQ(seen[static_cast<size_t>(p) * per_producer + i].load(), fails(p, i) ? 0 : 1) << p << "/" << i;
    }
  }
  EXPECT_EQ(q.Size(), 0u);
}

// BQ-22: a constructor that throws leaves a skipped ticket holding a
// capacity slot. A consumer already asleep must be woken to step over it,
// so a producer that comes later gets its item through.
template <class T>
void SkippedTicketsWithSleepingConsumer(size_t capacity) {
  BoundedBlockingQueue<T> q(capacity);
  std::atomic<int64_t> got{-1};
  std::thread consumer([&] {
    T out;
    if (q.Pop(out)) got = out.value();
  });
  std::this_thread::sleep_for(kSettle);
  for (size_t i = 0; i < capacity; ++i) EXPECT_THROW(q.Emplace(int64_t{100}, true), std::runtime_error);
  std::thread producer([&] { EXPECT_TRUE(q.Emplace(int64_t{7}, false)); });
  const bool delivered = Eventually([&] { return got.load() == 7; });
  EXPECT_TRUE(delivered) << "capacity " << capacity;
  if (!delivered) q.Abort();
  producer.join();
  consumer.join();
  EXPECT_EQ(q.Size(), 0u);
  for (size_t i = 0; i < capacity; ++i) EXPECT_TRUE(q.TryEmplace(int64_t{1}, false));
}

TEST(BoundedBlockingQueueTest, SkippedTicketWakesSleepingConsumer) {
  for (size_t capacity : {size_t{1}, size_t{4}, size_t{1000}}) {
    SkippedTicketsWithSleepingConsumer<TicketedItem>(capacity);
    SkippedTicketsWithSleepingConsumer<LockedItem>(capacity);
  }
}

// BQ-23: the other order: skipped tickets fill the queue first, a producer
// sleeps on the full queue, then a consumer arrives and must free the slots
// it steps over.
template <class T>
void SkippedTicketsWithSleepingProducer(size_t capacity) {
  BoundedBlockingQueue<T> q(capacity);
  for (size_t i = 0; i < capacity; ++i) EXPECT_THROW(q.Emplace(int64_t{100}, true), std::runtime_error);
  EXPECT_EQ(q.Size(), 0u);
  std::atomic<bool> pushed{false};
  std::thread producer([&] { pushed = q.Emplace(int64_t{7}, false); });
  std::this_thread::sleep_for(kSettle);
  EXPECT_FALSE(pushed.load());  // the skipped tickets still hold the slots
  std::atomic<int64_t> got{-1};
  std::thread consumer([&] {
    T out;
    if (q.Pop(out)) got = out.value();
  });
  const bool delivered = Eventually([&] { return got.load() == 7 && pushed.load(); });
  EXPECT_TRUE(delivered) << "capacity " << capacity;
  if (!delivered) q.Abort();
  producer.join();
  consumer.join();
}

TEST(BoundedBlockingQueueTest, SkippedTicketWakesSleepingProducer) {
  for (size_t capacity : {size_t{1}, size_t{4}, size_t{1000}}) {
    SkippedTicketsWithSleepingProducer<TicketedItem>(capacity);
    SkippedTicketsWithSleepingProducer<LockedItem>(capacity);
  }
}

// BQ-27: concurrent throwing constructors, more producers than lanes,
// bounded and unbounded, both consumer paths.
TEST(BoundedBlockingQueueTest, ConcurrentThrowingConstructorsExactMultiset) {
  for (size_t capacity : {BoundedBlockingQueue<int>::kUnbounded, size_t{1}, size_t{8}}) {
    {
      BoundedBlockingQueue<TicketedItem> q(capacity);
      RunMpmc(q, 10, 4, 2000, 5);
    }
    {
      BoundedBlockingQueue<LockedItem> q(capacity);
      RunMpmc(q, 10, 4, 2000, 5);
    }
  }
  EXPECT_EQ(TicketedItem::live.load(), 0);
  EXPECT_EQ(LockedItem::live.load(), 0);
}

// BQ-28: locked consumer path (throwing move) under MPMC load.
TEST(BoundedBlockingQueueTest, LockedPathMpmcStress) {
  for (size_t capacity : {BoundedBlockingQueue<int>::kUnbounded, size_t{64}}) {
    BoundedBlockingQueue<LockedItem> q(capacity);
    RunMpmc(q, 4, 4, 10000, 0);
  }
  EXPECT_EQ(LockedItem::live.load(), 0);
}

// BQ-29: heap-owning items through both paths; items still queued when the
// queue is destroyed are destroyed exactly once.
template <class T>
void HeapOwningMpmc() {
  {
    BoundedBlockingQueue<T> q;
    RunMpmc(q, 4, 4, 5000, 0);
    q.Reset();
    for (int i = 0; i < 300; ++i) ASSERT_TRUE(q.Emplace(int64_t{i}, false));
    T out;
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(q.TryPop(out));
  }
  EXPECT_EQ(T::live.load(), 0);
}

TEST(BoundedBlockingQueueTest, HeapOwningMpmc) {
  HeapOwningMpmc<TicketedItem>();
  HeapOwningMpmc<LockedItem>();
}

// BQ-30: Abort/Reset, SetCapacity and Clear racing blocked Push and Pop.
// Without Clear every successful Push is popped or left queued; nothing
// hangs and nothing leaks.
template <class T>
void AbortResetClearStorm(bool with_clear) {
  {
    BoundedBlockingQueue<T> q(4);
    std::atomic<bool> stop{false};
    std::atomic<long> pushed{0};
    std::atomic<long> popped{0};
    std::vector<std::thread> threads;
    for (int p = 0; p < 4; ++p) {
      threads.emplace_back([&] {
        for (int64_t i = 0; !stop; ++i) {
          if (q.Emplace(i, false)) {
            ++pushed;
          } else {
            std::this_thread::yield();
          }
        }
      });
    }
    for (int c = 0; c < 4; ++c) {
      threads.emplace_back([&] {
        T out;
        while (!stop) {
          if (q.Pop(out)) {
            ++popped;
          } else {
            std::this_thread::yield();
          }
        }
      });
    }
    std::mt19937 rng(1);
    for (int k = 0; k < 300; ++k) {
      switch (rng() % 3) {
        case 0:
          q.Abort();
          std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
          q.Reset();
          break;
        case 1:
          q.SetCapacity(1 + rng() % 8);
          break;
        default:
          if (with_clear) q.Clear();
          break;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 200));
    }
    stop = true;
    q.Abort();
    for (auto& t : threads) t.join();
    T out;
    long rest = 0;
    while (q.TryPop(out)) ++rest;
    if (!with_clear) {
      EXPECT_EQ(pushed.load(), popped.load() + rest);
    }
    EXPECT_GT(popped.load(), 0);
  }
  EXPECT_EQ(T::live.load(), 0);
}

TEST(BoundedBlockingQueueTest, AbortResetClearRaceBlockedCalls) {
  for (bool with_clear : {false, true}) {
    AbortResetClearStorm<TicketedItem>(with_clear);
    AbortResetClearStorm<LockedItem>(with_clear);
  }
}

// Whether thread `tid` of this process is asleep (state S in its /proc stat;
// a spinning or yielding thread is R).
bool ThreadSleeping(long tid) {
  std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/stat");
  std::string stat;
  std::getline(in, stat);
  const auto paren = stat.rfind(')');  // the name in (...) may hold spaces
  return paren != std::string::npos && paren + 2 < stat.size() && stat[paren + 2] == 'S';
}

// BQ-31: Reset() right after Abort() must not swallow the wakeup: every call
// blocked at the time still returns false.
TEST(BoundedBlockingQueueTest, AbortThenResetWakesAllBlocked) {
  BoundedBlockingQueue<int> empty;
  BoundedBlockingQueue<int> full(1);
  ASSERT_TRUE(full.Push(0));
  std::atomic<int> returned{0};
  std::atomic<int> succeeded{0};
  std::vector<std::atomic<long>> tids(12);
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&, i] {
      tids[i] = syscall(SYS_gettid);
      int v;
      succeeded += empty.Pop(v) ? 1 : 0;
      ++returned;
    });
  }
  for (int i = 8; i < 12; ++i) {
    threads.emplace_back([&, i] {
      tids[i] = syscall(SYS_gettid);
      succeeded += full.Push(1) ? 1 : 0;
      ++returned;
    });
  }
  // All 12 asleep in the queue, not still spinning. Several polls in a row:
  // a thread also sleeps briefly on a contended lock on its way there.
  int asleep_polls = 0;
  const auto give_up = std::chrono::steady_clock::now() + 10s;
  while (asleep_polls < 5 && std::chrono::steady_clock::now() < give_up) {
    bool all = true;
    for (auto& tid : tids) all = all && tid.load() != 0 && ThreadSleeping(tid.load());
    asleep_polls = all ? asleep_polls + 1 : 0;
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(asleep_polls, 5) << "not all 12 calls blocked";
  empty.Abort();
  empty.Reset();
  full.Abort();
  full.Reset();
  EXPECT_TRUE(Eventually([&] { return returned.load() == 12; }, 5s)) << returned.load() << " of 12 returned";
  // Release any call that missed the wakeup, so the test fails instead of hanging.
  empty.Abort();
  full.Abort();
  for (auto& t : threads) t.join();
  EXPECT_EQ(succeeded.load(), 0);
  empty.Reset();
  full.Reset();
  EXPECT_TRUE(empty.Push(5));
  int v = 0;
  EXPECT_TRUE(empty.Pop(v));
  EXPECT_EQ(v, 5);
  EXPECT_EQ(full.Size(), 1u);
}

// BQ-32: capacity shrinking and growing while producers and consumers run.
TEST(BoundedBlockingQueueTest, SetCapacityUnderLoad) {
  BoundedBlockingQueue<TicketedItem> q(8);
  RunMpmc(q, 4, 4, 5000, 0, [&q](const std::atomic<bool>& stop) {
    std::mt19937 rng(7);
    while (!stop) {
      q.SetCapacity(1 + rng() % 16);
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
    }
    q.SetCapacity(8);
  });
  EXPECT_EQ(q.Capacity(), 8u);
}

// BQ-33: a producer stalled inside its constructor (e.g. preempted). PopFor
// keeps its deadline; a Pop waits for the item, and so does a TryPop when T
// moves without throwing (it takes the oldest ticket and then waits),
// otherwise TryPop returns false at once. Nobody waiting burns CPU, not even
// the producer queued behind the stalled one in its lane.
struct Gate {
  std::mutex mu;
  std::condition_variable cv;
  bool open = false;
  void Wait() {
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [this] { return open; });
  }
  void Open() {
    {
      std::lock_guard<std::mutex> lock(mu);
      open = true;
    }
    cv.notify_all();
  }
};

template <bool kNothrowMove>
struct Stalling {
  int v = -1;
  Stalling() = default;
  Stalling(int x, Gate* gate) : v(x) {
    if (gate != nullptr) gate->Wait();
  }
  Stalling(Stalling&& o) noexcept(kNothrowMove) : v(o.v) {}
  Stalling& operator=(Stalling&& o) noexcept(kNothrowMove) {
    v = o.v;
    return *this;
  }
};

template <bool kNothrowMove>
void StalledProducer() {
  BoundedBlockingQueue<Stalling<kNothrowMove>> q;
  Gate gate;
  std::thread stalled([&] { EXPECT_TRUE(q.Emplace(0, &gate)); });
  ASSERT_TRUE(Eventually([&] { return q.Size() == 1; }));
  // Tickets 1..16; ticket 8 shares the stalled ticket's lane and waits for it.
  std::atomic<double> producer_cpu{-1};
  std::thread producer([&] {
    const double c0 = ThreadCpuMs();
    for (int i = 1; i <= 16; ++i) EXPECT_TRUE(q.Emplace(i, nullptr));
    producer_cpu = ThreadCpuMs() - c0;
  });
  ASSERT_TRUE(Eventually([&] { return q.Size() >= 9; }));

  // Blocking calls run on their own threads, so one that waits too long
  // fails the test instead of hanging it.
  std::atomic<int64_t> popfor_ms{-1};
  std::atomic<bool> popfor_result{true};
  std::thread popfor([&] {
    Stalling<kNothrowMove> o;
    const auto s = std::chrono::steady_clock::now();
    popfor_result = q.PopFor(o, 50ms);
    popfor_ms = ElapsedMs(s);
  });
  EXPECT_TRUE(Eventually([&] { return popfor_ms.load() >= 0; }, 5s)) << "PopFor missed its deadline";
  popfor.join();
  EXPECT_FALSE(popfor_result.load());
  EXPECT_GE(popfor_ms.load(), 50);
  EXPECT_LT(popfor_ms.load(), 2000);

  // -1: still waiting; -2: returned false; else the value taken.
  std::atomic<int> pop_got{-1};
  std::atomic<int> try_pop_got{-1};
  std::atomic<double> pop_cpu{-1};
  std::atomic<double> try_pop_cpu{-1};
  std::thread popper([&] {
    Stalling<kNothrowMove> o;
    const double c0 = ThreadCpuMs();
    const bool ok = q.Pop(o);
    pop_cpu = ThreadCpuMs() - c0;
    pop_got = ok ? o.v : -2;
  });
  std::thread try_popper([&] {
    Stalling<kNothrowMove> o;
    const double c0 = ThreadCpuMs();
    const bool ok = q.TryPop(o);
    try_pop_cpu = ThreadCpuMs() - c0;
    try_pop_got = ok ? o.v : -2;
  });
  if constexpr (!kNothrowMove) {
    EXPECT_TRUE(Eventually([&] { return try_pop_got.load() != -1; }, 1s)) << "TryPop waited";
    EXPECT_EQ(try_pop_got.load(), -2);
  }
  std::this_thread::sleep_for(300ms);
  // With a nothrow move TryPop takes ticket 0 (Pop only removes a ticket
  // once its item is there) and waits; Pop moves on to item 1, at once or
  // when its wait at ticket 0's lane ends. Otherwise Pop waits for item 0.
  if constexpr (kNothrowMove) {
    EXPECT_EQ(try_pop_got.load(), -1);
    EXPECT_TRUE(pop_got.load() == -1 || pop_got.load() == 1) << pop_got.load();
  } else {
    EXPECT_EQ(pop_got.load(), -1);
  }
  gate.Open();
  stalled.join();
  producer.join();
  popper.join();
  try_popper.join();

  int next = 1;
  if constexpr (kNothrowMove) {
    EXPECT_EQ(try_pop_got.load(), 0);
    EXPECT_EQ(pop_got.load(), 1);
    next = 2;
  } else {
    EXPECT_EQ(pop_got.load(), 0);
  }
  // Sleeping costs well under a millisecond; a busy wait would use most of
  // the ~300 ms.
  EXPECT_LT(pop_cpu.load(), 100.0);
  EXPECT_LT(try_pop_cpu.load(), 100.0);
  EXPECT_LT(producer_cpu.load(), 100.0);
  Stalling<kNothrowMove> out;
  for (int i = next; i <= 16; ++i) {
    ASSERT_TRUE(q.TryPop(out));
    EXPECT_EQ(out.v, i);
  }
  EXPECT_TRUE(q.Empty());
}

TEST(BoundedBlockingQueueTest, StalledProducerKeepsDeadlinesAndDoesNotBurnCpu) {
  StalledProducer<true>();
  StalledProducer<false>();
}

// BQ-34: a timeout too large for the clock (now + timeout overflows) means
// no timeout, not an immediate one.
template <class Duration>
void PopForHugeTimeout(Duration timeout) {
  BoundedBlockingQueue<int> q;
  // Start the clock before the pusher's: the push comes >= kSettle later.
  const auto start = std::chrono::steady_clock::now();
  std::thread pusher([&] {
    std::this_thread::sleep_for(kSettle);
    EXPECT_TRUE(q.Push(1));
  });
  int v = 0;
  EXPECT_TRUE(q.PopFor(v, timeout));
  EXPECT_GE(ElapsedMs(start), 40);
  EXPECT_EQ(v, 1);
  pusher.join();
}

TEST(BoundedBlockingQueueTest, PopForHugeTimeoutBlocks) {
  PopForHugeTimeout(std::chrono::hours::max());
  PopForHugeTimeout(std::chrono::nanoseconds::max());
  PopForHugeTimeout(std::chrono::seconds::max());
  PopForHugeTimeout(std::chrono::duration<double>::max());
  // Every branch of the range check: narrow and unsigned ticks, ticks finer
  // than the clock's, a ratio that is neither, floating ticks.
  PopForHugeTimeout(std::chrono::duration<std::int32_t>::max());
  PopForHugeTimeout(std::chrono::duration<std::uint64_t, std::milli>::max());
  PopForHugeTimeout(std::chrono::duration<std::int64_t, std::pico>::max());
  PopForHugeTimeout(std::chrono::duration<std::int64_t, std::ratio<1, 3>>::max());
  PopForHugeTimeout(std::chrono::duration<float, std::ratio<3600>>::max());
  PopForHugeTimeout(std::chrono::duration<long double>::max());
}

// ... and the most negative timeouts mean an immediate one, not overflow.
TEST(BoundedBlockingQueueTest, PopForMostNegativeTimeoutReturnsAtOnce) {
  BoundedBlockingQueue<int> q;
  int v = 0;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(q.PopFor(v, std::chrono::nanoseconds::min()));
  EXPECT_FALSE(q.PopFor(v, std::chrono::hours::min()));
  EXPECT_FALSE(q.PopFor(v, std::chrono::duration<std::int64_t, std::pico>::min()));
  EXPECT_FALSE(q.PopFor(v, std::chrono::duration<double>(-1e300)));
  EXPECT_FALSE(q.PopFor(v, std::chrono::seconds::zero()));
  EXPECT_FALSE(q.PopFor(v, std::chrono::duration<double>(std::numeric_limits<double>::quiet_NaN())));
  EXPECT_LT(ElapsedMs(start), 1000);
}

// BQ-35: producers often slow inside the constructor, consumers mixing Pop,
// TryPop and PopFor with short deadlines. The lock-free consumers own a lane
// in two ways (TryPop takes the ticket first and then waits for the lane;
// Pop and PopFor take the lane first and may let go of it at a deadline),
// which must never lose, duplicate or reorder an item. Half the consumers
// only Pop; the others stop once the producers are done, so the last items
// are left to blocking Pop calls and a lost wakeup there is not retried
// away by a TryPop or PopFor loop.
template <bool kNothrowMove>
struct Slow {
  int64_t v = -1;
  Slow() = default;
  explicit Slow(int64_t x) : v(x) {
    if (x % 5 == 0) std::this_thread::sleep_for(std::chrono::microseconds(x % 40));
  }
  Slow(Slow&& o) noexcept(kNothrowMove) : v(o.v) {}
  Slow& operator=(Slow&& o) noexcept(kNothrowMove) {
    v = o.v;
    return *this;
  }
};

template <bool kNothrowMove>
void SlowProducersMixedConsumers(size_t capacity) {
  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr int kPerProducer = 3000;
  BoundedBlockingQueue<Slow<kNothrowMove>> q(capacity);
  std::vector<std::atomic<int>> seen(size_t{kProducers} * kPerProducer);
  std::atomic<int> got{0};
  std::atomic<int> order_errors{0};
  std::atomic<bool> produced{false};
  std::vector<std::thread> threads;
  for (int c = 0; c < kConsumers; ++c) {
    threads.emplace_back([&, c] {
      std::mt19937 rng(static_cast<unsigned>(c + 11));
      std::vector<int64_t> last(kProducers, -1);
      Slow<kNothrowMove> out;
      const auto record = [&] {
        const auto p = static_cast<size_t>(out.v / kPerProducer);
        if (last[p] >= out.v % kPerProducer) ++order_errors;
        last[p] = out.v % kPerProducer;
        seen[static_cast<size_t>(out.v)].fetch_add(1);
        ++got;
      };
      const bool pop_only = c % 2 == 0;
      while (pop_only ? !q.Aborted() : !produced) {
        bool ok = false;
        switch (pop_only ? 0 : rng() % 3) {
          case 0:
            ok = q.Pop(out);
            break;
          case 1:
            ok = q.TryPop(out);
            break;
          default:
            ok = q.PopFor(out, std::chrono::microseconds(rng() % 30));
            break;
        }
        if (ok) record();
      }
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) EXPECT_TRUE(q.Emplace(int64_t{p} * kPerProducer + i));
    });
  }
  for (auto& t : producers) t.join();
  produced = true;
  EXPECT_TRUE(Eventually([&] { return got.load() == kProducers * kPerProducer; }, 60s)) << got.load();
  q.Abort();
  for (auto& t : threads) t.join();
  EXPECT_EQ(got.load(), kProducers * kPerProducer);
  EXPECT_EQ(order_errors.load(), 0);
  for (size_t i = 0; i < seen.size(); ++i) ASSERT_EQ(seen[i].load(), 1) << i;
  EXPECT_TRUE(q.Empty());
}

TEST(BoundedBlockingQueueTest, SlowProducersMixedConsumers) {
  for (size_t capacity : {BoundedBlockingQueue<int>::kUnbounded, size_t{3}}) {
    SlowProducersMixedConsumers<true>(capacity);
    SlowProducersMixedConsumers<false>(capacity);
  }
}

// BQ-38: a push signals one sleeping Pop. If that was a PopFor which then
// cannot take an item before its deadline (the oldest one is still being
// written), it must pass the signal on: two Pop calls asleep at the same
// time still get both items. The PopFor falls asleep first, so it is the
// one signalled (futex wakeups go in order). Getting all three asleep can
// take longer than the PopFor timeout on a slow machine (TSAN, loaded CI);
// then the run proves nothing and is repeated with twice the timeout.
bool StayAsleep(const std::vector<std::atomic<long>*>& tids) {
  int polls = 0;
  const auto give_up = std::chrono::steady_clock::now() + 10s;
  while (polls < 5 && std::chrono::steady_clock::now() < give_up) {
    bool all = true;
    for (auto* tid : tids) all = all && tid->load() != 0 && ThreadSleeping(tid->load());
    polls = all ? polls + 1 : 0;
    std::this_thread::sleep_for(10ms);
  }
  return polls == 5;
}

// False if the PopFor timed out before item 1 came (inconclusive).
template <bool kNothrowMove>
bool PopForPassesWakeupOnce(std::chrono::seconds timeout) {
  using T = Stalling<kNothrowMove>;
  BoundedBlockingQueue<T> q;
  std::atomic<long> popfor_tid{0};
  std::atomic<int> popfor_result{-1};  // -1: still waiting
  std::chrono::steady_clock::duration popfor_took{};
  std::thread popfor([&] {
    popfor_tid = syscall(SYS_gettid);
    T o;
    const auto start = std::chrono::steady_clock::now();
    const bool got = q.PopFor(o, timeout);
    popfor_took = std::chrono::steady_clock::now() - start;
    popfor_result = got ? 1 : 0;
  });
  // EXPECT rather than ASSERT from here on: the threads must be joined.
  EXPECT_TRUE(StayAsleep({&popfor_tid}));
  std::vector<std::atomic<long>> pop_tids(2);
  std::vector<std::atomic<int>> popped(2);  // -1: still waiting; -2: false
  std::vector<std::thread> pops;
  for (size_t i = 0; i < 2; ++i) {
    popped[i] = -1;
    pops.emplace_back([&, i] {
      pop_tids[i] = syscall(SYS_gettid);
      T o;
      popped[i] = q.Pop(o) ? o.v : -2;
    });
  }
  EXPECT_TRUE(StayAsleep({&popfor_tid, &pop_tids[0], &pop_tids[1]}));

  // Item 0 stays in its constructor; item 1 is published and signals.
  Gate gate;
  std::thread stalled([&] { EXPECT_TRUE(q.Emplace(0, &gate)); });
  EXPECT_TRUE(Eventually([&] { return q.Size() == 1; }));
  EXPECT_TRUE(q.Emplace(1, nullptr));
  if (popfor_result.load() != -1) {  // timed out before item 1 came
    gate.Open();
    stalled.join();
    q.Abort();
    popfor.join();
    for (auto& t : pops) t.join();
    EXPECT_EQ(popfor_result.load(), 0);
    EXPECT_GE(popfor_took, timeout) << "PopFor returned false before its deadline";
    return false;
  }
  EXPECT_TRUE(Eventually([&] { return popfor_result.load() != -1; }, timeout + 5s));
  EXPECT_EQ(popfor_result.load(), 0);
  gate.Open();
  stalled.join();
  EXPECT_TRUE(Eventually([&] { return popped[0].load() != -1 && popped[1].load() != -1; }, 5s))
      << "a Pop slept with an item queued";
  q.Abort();  // releases a Pop that missed its wakeup, so the test fails instead of hanging
  popfor.join();
  for (auto& t : pops) t.join();
  EXPECT_EQ(popped[0].load() + popped[1].load(), 1);
  EXPECT_EQ(std::min(popped[0].load(), popped[1].load()), 0);
  EXPECT_TRUE(q.Empty());
  return true;
}

template <bool kNothrowMove>
void PopForPassesWakeupOn() {
  for (auto timeout = 1s; timeout <= 16s; timeout *= 2) {
    if (PopForPassesWakeupOnce<kNothrowMove>(timeout)) return;
  }
  ADD_FAILURE() << "PopFor timed out before item 1 came even with a 16 s timeout";
}

TEST(BoundedBlockingQueueTest, PopForTimeoutPassesWakeupOn) {
  PopForPassesWakeupOn<true>();
  PopForPassesWakeupOn<false>();
}

// BQ-40: the same for a Pop whose move assignment throws (locked consumer
// path): the item stays queued, the exception leaves Pop(), and the other
// sleeping Pop must still get the item.
struct ThrowsOnAssign {
  static inline std::atomic<bool> throw_next{false};
  int v = -1;
  ThrowsOnAssign() = default;
  explicit ThrowsOnAssign(int x) : v(x) {}
  ThrowsOnAssign(ThrowsOnAssign&& o) noexcept(false) : v(o.v) {}
  ThrowsOnAssign& operator=(ThrowsOnAssign&& o) noexcept(false) {
    if (throw_next.exchange(false)) throw std::runtime_error("assign");
    v = o.v;
    return *this;
  }
};

TEST(BoundedBlockingQueueTest, ThrowingPopPassesWakeupOn) {
  BoundedBlockingQueue<ThrowsOnAssign> q;
  std::vector<std::atomic<long>> tids(2);
  std::vector<std::atomic<int>> popped(2);  // -1: still waiting; -2: false; -3: threw
  std::vector<std::thread> pops;
  for (size_t i = 0; i < 2; ++i) {
    popped[i] = -1;
    pops.emplace_back([&, i] {
      tids[i] = syscall(SYS_gettid);
      ThrowsOnAssign o;
      try {
        popped[i] = q.Pop(o) ? o.v : -2;
      } catch (const std::runtime_error&) {
        popped[i] = -3;
      }
    });
  }
  // EXPECT rather than ASSERT: the threads must be joined.
  EXPECT_TRUE(StayAsleep({&tids[0], &tids[1]}));
  ThrowsOnAssign::throw_next = true;
  EXPECT_TRUE(q.Emplace(7));
  EXPECT_TRUE(Eventually([&] { return popped[0].load() != -1 && popped[1].load() != -1; }, 5s))
      << "a Pop slept with an item queued";
  q.Abort();  // releases a Pop that missed its wakeup
  for (auto& t : pops) t.join();
  EXPECT_FALSE(ThrowsOnAssign::throw_next.load());
  EXPECT_EQ(std::min(popped[0].load(), popped[1].load()), -3);
  EXPECT_EQ(std::max(popped[0].load(), popped[1].load()), 7);
  EXPECT_TRUE(q.Empty());
}

// BQ-41: one producer pushes bursts with gaps of varying length to
// consumers blocking in Pop, so consumers both spin and sleep, pushes skip
// waking sleepers while one spins, and the number of yields before
// sleeping both grows and shrinks. Every item comes out exactly once, and
// after about one burst in four the queue drains. With capacity 4 the producer
// waits too; a watchdog aborts the queue if a Push never returns.
TEST(BoundedBlockingQueueTest, BurstsReachSleepingConsumers) {
  for (size_t capacity : {BoundedBlockingQueue<int>::kUnbounded, size_t{4}}) {
    constexpr int kConsumers = 8;
    constexpr int kItems = 20000;
    BoundedBlockingQueue<int> q(capacity);
    std::vector<std::atomic<int>> seen(kItems);
    std::atomic<int> got{0};
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
      consumers.emplace_back([&] {
        int v = -1;
        while (q.Pop(v)) {
          seen[static_cast<size_t>(v)].fetch_add(1);
          ++got;
        }
      });
    }
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
      if (!Eventually([&] { return done.load(); }, 60s)) q.Abort();
    });
    // EXPECT rather than ASSERT until the threads are joined.
    std::mt19937 rng(41);
    bool ok = true;  // every Push returned and the queue drained
    for (int i = 0; i < kItems && ok;) {
      for (int burst = 1 + static_cast<int>(rng() % 32); burst > 0 && i < kItems && ok; --burst, ++i) {
        ok = q.Push(i);
        EXPECT_TRUE(ok) << "capacity " << capacity << ": Push " << i << " did not return in 60 s";
      }
      if (!ok) break;
      switch (rng() % 4) {
        case 0:
          break;
        case 1:
          std::this_thread::yield();
          break;
        case 2:
          std::this_thread::sleep_for(20us);
          break;
        default:
          ok = Eventually([&] { return got.load() == i; });
          EXPECT_TRUE(ok) << "capacity " << capacity << ": " << i - got.load() << " items left queued";
          std::this_thread::sleep_for(300us);
          break;
      }
    }
    if (ok) {
      EXPECT_TRUE(Eventually([&] { return got.load() == kItems; })) << "got " << got.load() << " of " << kItems;
    }
    done = true;
    watchdog.join();
    q.Abort();
    for (auto& t : consumers) t.join();
    if (!ok) continue;
    for (int i = 0; i < kItems; ++i) ASSERT_EQ(seen[static_cast<size_t>(i)].load(), 1) << i;
  }
}

// BQ-42: a push need not wake a sleeping Pop while another Pop spins (see
// Wake()); here each consumer pops once and leaves, so no other Pop would
// take an item whose wake was skipped wrongly. The pushes come while
// consumers spin, sleep, or are about to.
TEST(BoundedBlockingQueueTest, PopsThatSpunPassWakeupOn) {
  constexpr int kConsumers = 4;
  std::mt19937 rng(42);
  for (int trial = 0; trial < 2000; ++trial) {
    BoundedBlockingQueue<int> q;
    std::atomic<int> got{0};
    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
      consumers.emplace_back([&] {
        int v = -1;
        if (q.Pop(v)) ++got;
      });
    }
    switch (rng() % 3) {
      case 0:
        break;
      case 1:
        std::this_thread::sleep_for(std::chrono::microseconds(rng() % 100));
        break;
      default:
        std::this_thread::sleep_for(2ms);
        break;
    }
    for (int i = 0; i < kConsumers; ++i) {
      EXPECT_TRUE(q.Push(i));
      if (rng() % 2 == 0) std::this_thread::yield();
    }
    const bool all = Eventually([&] { return got.load() == kConsumers; }, 5s);
    EXPECT_TRUE(all) << "trial " << trial << ": " << kConsumers - got.load() << " items left queued";
    q.Abort();  // releases the Pop calls that missed their wakeup
    for (auto& t : consumers) t.join();
    if (!all) return;
  }
}

// Whether a SCHED_IDLE thread next to a busy one on its CPU still runs
// every few milliseconds, as on OHOS (HongMeng kernel; on Linux it gets
// almost no CPU). A starved consumer (below) is then a slow one: the items
// whose wake or turn falls to it are late more often, with tbb too. On
// the board (2026-11), items late in BQ-46 at a 3000 us gap: mean 66 of 700
// in 16 runs (in 13 runs tbb 223, a mutex and condition variable queue 226),
// at most 82 in 20 runs of the test; in BQ-47 with two starved consumers, 30
// runs: mean 8.4 of 200, max 14 (tbb mean 7.8, max 15), at most 20 in 20 runs
// of the test. The late counts allowed there are raised to fit.
#if defined(__OHOS__)
constexpr bool kIdleThreadsRun = true;
#else
constexpr bool kIdleThreadsRun = false;
#endif

// `count` consumers that pop until the queue is aborted on the last allowed
// CPU, as SCHED_IDLE, next to a thread that keeps that CPU busy: they get
// almost no CPU, so they are mostly preempted or in a yield while they wait
// in Pop. Each item taken goes to `take`. The constructing thread (the
// producer) keeps off that CPU until Join(); other threads of the test call
// LeaveCpu() for that. Needs 4 allowed CPUs, so that the other
// threads still have some to themselves on a loaded machine.
// Join() (or the destructor) aborts the queue. sched_setaffinity(0, ...)
// sets the calling thread's affinity: OHOS's libc has no
// pthread_setaffinity_np.
class StarvedConsumer {
 public:
  StarvedConsumer(BoundedBlockingQueue<int>& q, std::atomic<int>& got, int count = 1)
      : StarvedConsumer(q, [&got](int) { ++got; }, count) {}
  StarvedConsumer(BoundedBlockingQueue<int>& q, std::function<void(int)> take, int count)
      : q_(q), take_(std::move(take)), count_(count) {
    CPU_ZERO(&allowed_);
    if (sched_getaffinity(0, sizeof(allowed_), &allowed_) != 0 || CPU_COUNT(&allowed_) < 4) {
      failed_ = true;
      return;
    }
    int cpu = CPU_SETSIZE - 1;
    while (!CPU_ISSET(cpu, &allowed_)) --cpu;
    CPU_ZERO(&one_);
    CPU_SET(cpu, &one_);
    rest_ = allowed_;
    CPU_CLR(cpu, &rest_);
    left_ = sched_setaffinity(0, sizeof(rest_), &rest_) == 0;
    hog_ = std::thread([this] {
      if (sched_setaffinity(0, sizeof(one_), &one_) != 0) failed_ = true;
      ++ready_;
      while (!stop_.load(std::memory_order_relaxed)) {
      }
    });
    try {
      for (int c = 0; c < count; ++c) {
        consumers_.emplace_back([this] {
          sched_param param{};
          if (sched_setaffinity(0, sizeof(one_), &one_) != 0 ||
              pthread_setschedparam(pthread_self(), SCHED_IDLE, &param) != 0) {
            failed_ = true;
          }
          ++ready_;
          int v = -1;
          while (q_.Pop(v)) take_(v);
        });
      }
    } catch (...) {
      Join();
      throw;
    }
  }
  StarvedConsumer(const StarvedConsumer&) = delete;
  StarvedConsumer& operator=(const StarvedConsumer&) = delete;
  ~StarvedConsumer() { Join(); }

  // False if there are fewer than 4 CPUs, or a thread could not be pinned or
  // made SCHED_IDLE.
  bool SetUp() {
    return !failed_.load() && Eventually([this] { return ready_.load() == count_ + 1; }) && !failed_.load();
  }

  // Best effort: a thread left on that CPU only makes the test stricter.
  void LeaveCpu() const {
    if (!failed_.load()) sched_setaffinity(0, sizeof(rest_), &rest_);
  }

  // Called by the constructing thread.
  void Join() {
    q_.Abort();
    stop_ = true;
    if (hog_.joinable()) hog_.join();
    for (auto& t : consumers_) t.join();
    consumers_.clear();
    if (left_) sched_setaffinity(0, sizeof(allowed_), &allowed_);
    left_ = false;
  }

 private:
  // Held while alive: waits until no other process (a parallel ctest run
  // of this test binary) has its starved consumers and hog on the last
  // CPU, where they would compete with these: then starved consumers there
  // get the CPU now and then and take items late, with tbb too. Best
  // effort: gives up waiting after kWait.
  class CpuLock {
   public:
    CpuLock() {
      constexpr auto kWait = std::chrono::minutes(2);
      std::error_code ec;
      const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
      if (ec) return;
      // Read-only, so that other users can open the file too (umask).
      fd_ = open((dir / "ibmw_selfdev_starved_consumer.lock").c_str(), O_RDONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                 0666);
      const auto end = std::chrono::steady_clock::now() + kWait;
      while (fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        if ((errno != EWOULDBLOCK && errno != EINTR) || std::chrono::steady_clock::now() >= end) {
          close(fd_);
          fd_ = -1;
        } else {
          std::this_thread::sleep_for(10ms);
        }
      }
    }
    CpuLock(const CpuLock&) = delete;
    CpuLock& operator=(const CpuLock&) = delete;
    ~CpuLock() {
      if (fd_ >= 0) close(fd_);  // releases the lock
    }

   private:
    int fd_ = -1;
  };

  const CpuLock lock_;  // first: taken before, released after the rest
  BoundedBlockingQueue<int>& q_;
  const std::function<void(int)> take_;
  const int count_;
  cpu_set_t allowed_;  // the constructing thread's affinity
  cpu_set_t one_;
  cpu_set_t rest_;
  bool left_ = false;  // whether that thread keeps off one_
  std::atomic<bool> failed_{false};
  std::atomic<int> ready_{0};
  std::atomic<bool> stop_{false};
  std::thread hog_;
  std::vector<std::thread> consumers_;
};

// BQ-43: a starved consumer waiting in Pop does not keep the others, asleep
// on other CPUs, from being woken: once a skipped wake held an item back,
// pushes wake sleepers again (see SkipGate). The last push is a spare: the
// starved consumer may have taken an item and not counted it yet.
TEST(BoundedBlockingQueueTest, StarvedSpinnerDoesNotHoldBackItems) {
  constexpr int kItems = 200;
  BoundedBlockingQueue<int> q;
  std::atomic<int> got{0};
  StarvedConsumer starved(q, got);
  std::vector<std::thread> consumers;
  for (int c = 0; c < 3; ++c) {
    consumers.emplace_back([&] {
      starved.LeaveCpu();
      int v = -1;
      while (q.Pop(v)) ++got;
    });
  }
  // EXPECT rather than ASSERT until the threads are joined.
  const bool set_up = starved.SetUp();
  bool all = false;
  if (set_up) {
    std::this_thread::sleep_for(kSettle);  // all consumers waiting
    for (int i = 0; i <= kItems; ++i) {
      EXPECT_TRUE(q.Push(i));
      std::this_thread::sleep_for(500us);
    }
    all = Eventually([&] { return q.Empty() && got.load() >= kItems; }, 1s);
  }
  q.Abort();
  starved.Join();
  for (auto& t : consumers) t.join();
  if (!set_up) GTEST_SKIP() << "needs 4 CPUs, a pinned thread and SCHED_IDLE";
  EXPECT_TRUE(all) << kItems - got.load() << " items left queued";
}

// BQ-44: pushes wake sleepers although a starved consumer is counted as
// spinning, at the latest after kBackstop. Here each of the other consumers
// pops once and leaves, so nothing else would take the items.
TEST(BoundedBlockingQueueTest, QuietQueueWakesSleepersDespiteStarvedSpinner) {
  for (int trial = 0; trial < 20; ++trial) {
    BoundedBlockingQueue<int> q;
    std::atomic<int> got{0};
    StarvedConsumer starved(q, got);
    std::vector<std::thread> consumers;
    for (int c = 0; c < 2; ++c) {
      consumers.emplace_back([&] {
        starved.LeaveCpu();
        int v = -1;
        if (q.Pop(v)) ++got;
      });
    }
    // EXPECT rather than ASSERT until the threads are joined.
    const bool set_up = starved.SetUp();
    bool all = false;
    if (set_up) {
      std::this_thread::sleep_for(kSettle);  // all consumers waiting
      EXPECT_TRUE(q.Push(1));
      EXPECT_TRUE(q.Push(2));
      all = Eventually([&] { return got.load() == 2; }, 200ms);
    }
    q.Abort();
    starved.Join();
    for (auto& t : consumers) t.join();
    if (!set_up) GTEST_SKIP() << "needs 4 CPUs, a pinned thread and SCHED_IDLE";
    ASSERT_TRUE(all) << "trial " << trial << ": " << 2 - got.load() << " items left queued";
  }
}

// BQ-45: a starved consumer waiting in Pop does not hold back the items of
// a producer that waits for each item to be taken before the next push, at
// any pace: once a skipped wake held an item back, pushes wake sleepers
// again (see SkipGate). Items still late (> 5 ms) are, but for that first
// one, ones the starved consumer was woken for and took itself, as with tbb.
TEST(BoundedBlockingQueueTest, StarvedSpinnerDoesNotStallPushAndWait) {
  constexpr int kRounds = 100;
  constexpr std::chrono::microseconds kGaps[] = {0us, 20us, 50us, 500us, 2000us};
  constexpr int kLateAllowed = kRounds / 10;
  BoundedBlockingQueue<int> q;
  std::atomic<int> got{0};
  StarvedConsumer starved(q, got);
  std::vector<std::thread> consumers;
  for (int c = 0; c < 3; ++c) {
    consumers.emplace_back([&] {
      starved.LeaveCpu();
      int v = -1;
      while (q.Pop(v)) ++got;
    });
  }
  // EXPECT rather than ASSERT until the threads are joined.
  const bool set_up = starved.SetUp();
  int taken = 0;
  int late = 0;
  if (set_up) {
    std::this_thread::sleep_for(kSettle);  // all consumers waiting
    for (; taken < kRounds; ++taken) {
      const auto start = std::chrono::steady_clock::now();
      EXPECT_TRUE(q.Push(taken));
      // Polled without sleeping, so that the next push can come while
      // consumers still spin.
      while (got.load() != taken + 1 && std::chrono::steady_clock::now() - start < 200ms) {
      }
      if (got.load() != taken + 1) break;
      if (std::chrono::steady_clock::now() - start > 5ms) ++late;
      std::this_thread::sleep_for(kGaps[taken % std::size(kGaps)]);
    }
  }
  q.Abort();
  starved.Join();
  for (auto& t : consumers) t.join();
  if (!set_up) GTEST_SKIP() << "needs 4 CPUs, a pinned thread and SCHED_IDLE";
  EXPECT_EQ(taken, kRounds) << "item " << taken << " not taken within 200 ms";
  EXPECT_LE(late, kLateAllowed) << late << " of " << kRounds << " items taken after more than 5 ms";
}

// BQ-46: nor those of a producer that pushes at a steady pace without
// waiting, fed to three consumers or one: a skip whose item is still queued
// when the next push comes (after kSkipGrace), or when a sleeper's kBackstop
// ends, turns skips off (see SkipGate), and the starved consumer waits in
// Pop mostly yielding (see kMinYields). Late: taken more than 2 ms after its
// push; up to 10% (15% for one consumer if kIdleThreadsRun).
TEST(BoundedBlockingQueueTest, StarvedSpinnerDoesNotStallSteadyPushes) {
  struct Case {
    std::chrono::microseconds gap;
    int consumers;
    int items;
  };
  for (const Case& c : {Case{500us, 3, 1000}, Case{3000us, 1, 700}}) {
    BoundedBlockingQueue<int> q;
    // Written before an item's push, read after its pop.
    std::vector<std::chrono::steady_clock::time_point> pushed_at(c.items);
    std::atomic<int> late{0};
    const auto take = [&](int v) {
      if (std::chrono::steady_clock::now() - pushed_at[v] > 2ms) ++late;
    };
    StarvedConsumer starved(q, take, 1);
    std::vector<std::thread> consumers;
    for (int i = 0; i < c.consumers; ++i) {
      consumers.emplace_back([&] {
        starved.LeaveCpu();
        int v = -1;
        while (q.Pop(v)) take(v);
      });
    }
    const bool set_up = starved.SetUp();
    if (set_up) {
      std::this_thread::sleep_for(kSettle);  // all consumers waiting
      for (int i = 0; i < c.items; ++i) {
        pushed_at[i] = std::chrono::steady_clock::now();
        EXPECT_TRUE(q.Push(i));
        std::this_thread::sleep_for(c.gap);
      }
      EXPECT_TRUE(Eventually([&] { return q.Empty(); })) << q.Size() << " items left queued";
    }
    q.Abort();
    starved.Join();
    for (auto& t : consumers) t.join();
    if (!set_up) GTEST_SKIP() << "needs 4 CPUs, a pinned thread and SCHED_IDLE";
    const int allowed = c.items * (kIdleThreadsRun && c.consumers == 1 ? 15 : 10) / 100;
    EXPECT_LE(late.load(), allowed)
        << "gap " << c.gap.count() << " us, " << c.consumers << " consumers: " << late.load() << " of " << c.items
        << " items late";
  }
}

// BQ-47: nor those of bursts after a lull, with one or two starved consumers
// on one CPU: a skip whose item is still queued when a sleeper's kBackstop
// ends turns skips off (see SkipGate), and the starved consumers wait in
// Pop mostly yielding, so that wakes go to consumers that run (see
// kMinYields), and are passed on only for skipped items, also by a
// signalled sleeper whose own item may be one (see PopUntil()). Late:
// taken more than 5 ms after its push; up to 5% (15% if kIdleThreadsRun).
TEST(BoundedBlockingQueueTest, StarvedSpinnerDoesNotDelayBursts) {
  constexpr int kRounds = 40;
  constexpr int kBurst = 5;
  constexpr int kLateAllowed = kRounds * kBurst * (kIdleThreadsRun ? 3 : 1) / 20;
  for (int starved_count : {1, 2}) {
    BoundedBlockingQueue<int> q;
    // Written before an item's push, read after its pop.
    std::vector<std::chrono::steady_clock::time_point> pushed_at(kRounds * kBurst);
    std::atomic<int> got{0};
    std::atomic<int> late{0};
    const auto take = [&](int v) {
      if (std::chrono::steady_clock::now() - pushed_at[v] > 5ms) ++late;
      ++got;
    };
    StarvedConsumer starved(q, take, starved_count);
    std::vector<std::thread> consumers;
    for (int c = 0; c < 3; ++c) {
      consumers.emplace_back([&] {
        starved.LeaveCpu();
        int v = -1;
        while (q.Pop(v)) take(v);
      });
    }
    const bool set_up = starved.SetUp();
    int pushed = 0;
    if (set_up) {
      std::this_thread::sleep_for(kSettle);  // all consumers waiting
      for (int r = 0; r < kRounds; ++r) {
        std::this_thread::sleep_for(30ms);  // sleepers settle, as in a lull
        for (int b = 0; b < kBurst; ++b) {
          pushed_at[pushed] = std::chrono::steady_clock::now();
          EXPECT_TRUE(q.Push(pushed++));
          if (b + 1 < kBurst) std::this_thread::sleep_for(200us);
        }
        const auto start = std::chrono::steady_clock::now();
        while (got.load() != pushed && std::chrono::steady_clock::now() - start < 200ms) {
        }
        EXPECT_EQ(got.load(), pushed) << "round " << r << " not taken within 200 ms";
      }
    }
    q.Abort();
    starved.Join();
    for (auto& t : consumers) t.join();
    if (!set_up) GTEST_SKIP() << "needs 4 CPUs, a pinned thread and SCHED_IDLE";
    EXPECT_LE(late.load(), kLateAllowed)
        << starved_count << " starved: " << late.load() << " of " << pushed << " items late";
  }
}

// BQ-48: a thread's flag that it gets little CPU (see kMinYields) is kept
// for its next wait, and a thread that shares the flag but does not get
// little CPU neither sees it nor clears it; one that does takes it over.
TEST(BoundedBlockingQueueTest, SlowFlagIsKeptPerThread) {
  using Flag = ibmw::selfdev::concurrent::detail::SlowFlag<6>;
  Flag::Flags flags = {};
  // Same flag (top 6 bits), other marks (next 7 bits); then another flag.
  constexpr uint64_t kA = (5ull << 58) | (0x11ull << 51);
  constexpr uint64_t kB = (5ull << 58) | (0x22ull << 51);
  constexpr uint64_t kOther = (6ull << 58) | (0x11ull << 51);
  const auto bound = [&flags](uint64_t hash) {
    Flag f;
    f.Bind(flags, hash);
    return f;
  };
  Flag unbound;
  unbound.Set(true);
  EXPECT_FALSE(unbound.Get());
  for (const auto& f : flags) EXPECT_EQ(f.load(), 0);

  Flag a = bound(kA);
  EXPECT_FALSE(a.Get());
  a.Set(true);
  EXPECT_TRUE(a.Get());
  EXPECT_TRUE(bound(kA).Get());  // its next wait
  EXPECT_FALSE(bound(kOther).Get());

  Flag b = bound(kB);
  EXPECT_FALSE(b.Get());
  b.Set(false);
  EXPECT_TRUE(bound(kA).Get());
  b.Set(true);  // also slow: takes the flag over
  EXPECT_FALSE(bound(kA).Get());
  a.Set(false);  // a's next wait clears only its own mark
  EXPECT_TRUE(bound(kB).Get());
  b.Set(false);
  EXPECT_FALSE(bound(kB).Get());
  EXPECT_FALSE(bound(kA).Get());

  Flag self;
  self.Bind(flags);  // this thread's own hash
  self.Set(true);
  EXPECT_TRUE(bound(Flag::ThreadHash()).Get());

  // Thread ids as on the OHOS board: addresses 0x103000 apart. Fibonacci
  // hashing put them in 5 flags. Mix spreads these; an arbitrary 8 may
  // still share flags.
  std::vector<uint64_t> picked;
  for (uint64_t k = 0; k < 8; ++k) picked.push_back(Flag::Mix(0x59fce89a60u + k * 0x103000u) >> 58);
  std::sort(picked.begin(), picked.end());
  EXPECT_EQ(std::unique(picked.begin(), picked.end()) - picked.begin(), 8);
}
}  // namespace bounded_blocking_queue_test
