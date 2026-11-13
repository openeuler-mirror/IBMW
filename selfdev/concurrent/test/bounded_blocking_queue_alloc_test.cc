// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Block allocation failures inside Push. Its own executable: it wraps the
// global operator new so that allocations fail on demand, per thread.
// Wrapped (ld --wrap) rather than replaced: Clang's static sanitizer
// runtimes define operator new themselves, so a replacement does not link.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ibmw_selfdev/concurrent/bounded_blocking_queue.h"

namespace {

// Allocations on this thread fail while set. kRandom fails about half of
// them, driven by `seed`.
enum class FailMode { kNone, kAll, kRandom };
thread_local FailMode fail_mode = FailMode::kNone;
thread_local uint32_t seed = 1;

bool ShouldFail() {
  switch (fail_mode) {
    case FailMode::kNone:
      return false;
    case FailMode::kAll:
      return true;
    case FailMode::kRandom:
      seed = seed * 1103515245u + 12345u;
      return ((seed >> 16) & 1) != 0;
  }
  return false;
}

}  // namespace

// Calls of operator new(std::size_t) from this executable's own code (the
// queue's block allocation among them) land here; see CMakeLists.txt.
extern "C" void* __real__Znwm(std::size_t n);

extern "C" void* __wrap__Znwm(std::size_t n) {
  if (ShouldFail()) throw std::bad_alloc();
  return __real__Znwm(n);
}

namespace bounded_blocking_queue_alloc_test {

using ibmw::selfdev::concurrent::BoundedBlockingQueue;
using namespace std::chrono_literals;

// A lane needs a new block at lane index 16 (ticket 128) unless it has a
// spare one; a queue that was never popped has none.
constexpr int kFirstBlockTicket = 128;

struct FailScope {
  explicit FailScope(FailMode mode) { fail_mode = mode; }
  ~FailScope() { fail_mode = FailMode::kNone; }
  FailScope(const FailScope&) = delete;
  FailScope& operator=(const FailScope&) = delete;
};

template <class Done>
bool Eventually(Done done, std::chrono::milliseconds limit = 10s) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// Throwing move: selects the locked consumer path.
struct LockedInt {
  int64_t v = -1;
  LockedInt() = default;
  explicit LockedInt(int64_t x) : v(x) {}
  LockedInt(LockedInt&& o) : v(o.v) {}
  LockedInt& operator=(LockedInt&& o) {
    v = o.v;
    return *this;
  }
};

int64_t Value(int64_t v) { return v; }
int64_t Value(const LockedInt& v) { return v.v; }

// BQ-24: runs of failed block allocations at every position, including
// several whole blocks in a row: survivors keep FIFO order, Size() matches,
// the destructor frees what is left (ASAN: no leak, no double free).
template <class T>
void FailedBlockRuns() {
  for (int start = 0; start < 400; start += 7) {
    for (int len : {1, 2, 17, 40}) {
      BoundedBlockingQueue<T> q;
      std::vector<int64_t> ok;
      size_t front = 0;
      for (int i = 0; i < 600; ++i) {
        bool pushed = false;
        try {
          FailScope fail(i >= start && i < start + len ? FailMode::kAll : FailMode::kNone);
          pushed = q.Push(T(i));
        } catch (const std::bad_alloc&) {
        }
        if (pushed) ok.push_back(i);
        if (i % 50 == 49) {
          T out;
          for (int j = 0; j < 10 && q.TryPop(out); ++j) {
            ASSERT_LT(front, ok.size());
            ASSERT_EQ(Value(out), ok[front++]) << "start " << start << " len " << len;
          }
        }
      }
      EXPECT_EQ(q.Size(), ok.size() - front);
      T out;
      while (q.TryPop(out)) {
        ASSERT_LT(front, ok.size());
        ASSERT_EQ(Value(out), ok[front++]);
      }
      EXPECT_EQ(front, ok.size());
      for (int i = 0; i < 300; ++i) ASSERT_TRUE(q.Push(T(i)));  // left for the destructor
    }
  }
}

TEST(BoundedBlockingQueueAllocTest, FailedBlockAllocationRuns) {
  FailedBlockRuns<int64_t>();
  FailedBlockRuns<LockedInt>();
}

// BQ-25: a push whose block allocation fails leaves a ticket with no cell,
// holding a capacity slot; sleeping consumers and producers must still be
// woken (cf. BQ-22 / BQ-23 for a throwing constructor).
template <class T>
void FailedBlockWithSleepers(bool consumer_first) {
  BoundedBlockingQueue<T> q(1);
  T out;
  for (int i = 0; i < kFirstBlockTicket; ++i) {
    ASSERT_TRUE(q.Push(T(i)));
    ASSERT_TRUE(q.TryPop(out));
  }
  std::atomic<int64_t> got{-1};
  std::atomic<bool> pushed{false};
  std::thread consumer;
  std::thread producer;
  const auto start_consumer = [&] {
    consumer = std::thread([&] {
      T o;
      if (q.Pop(o)) got = Value(o);
    });
  };
  if (consumer_first) {
    start_consumer();
    std::this_thread::sleep_for(50ms);
  }
  {
    FailScope fail(FailMode::kAll);
    EXPECT_THROW(q.Push(T(kFirstBlockTicket)), std::bad_alloc);
  }
  producer = std::thread([&] { pushed = q.Push(T(7)); });
  if (!consumer_first) {
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(pushed.load());
    start_consumer();
  }
  const bool delivered = Eventually([&] { return got.load() == 7 && pushed.load(); });
  EXPECT_TRUE(delivered) << "consumer first: " << consumer_first;
  if (!delivered) q.Abort();
  producer.join();
  consumer.join();
  EXPECT_EQ(q.Size(), 0u);
}

TEST(BoundedBlockingQueueAllocTest, FailedBlockAllocationWakesSleepers) {
  for (bool consumer_first : {true, false}) {
    FailedBlockWithSleepers<int64_t>(consumer_first);
    FailedBlockWithSleepers<LockedInt>(consumer_first);
  }
}

// BQ-26: producers whose block allocations fail about half the time, racing
// consumers: every successful push comes out exactly once, in per-producer
// order for each consumer.
template <class T>
void RandomFailuresMpmc(size_t capacity) {
  constexpr int kProducers = 6;
  constexpr int kConsumers = 3;
  constexpr int kPerProducer = 4000;
  BoundedBlockingQueue<T> q(capacity);
  std::vector<std::vector<uint8_t>> ok(kProducers, std::vector<uint8_t>(kPerProducer, 0));
  std::vector<std::atomic<int>> seen(size_t{kProducers} * kPerProducer);
  std::atomic<long> pushed{0};
  std::atomic<long> got{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      std::vector<int64_t> last(kProducers, -1);
      T out;
      const auto record = [&] {
        const int64_t v = Value(out);
        const auto p = static_cast<size_t>(v / kPerProducer);
        if (last[p] >= v % kPerProducer) ++errors;
        last[p] = v % kPerProducer;
        seen[static_cast<size_t>(v)].fetch_add(1);
        ++got;
      };
      while (q.Pop(out)) record();
      while (q.TryPop(out)) record();
    });
  }
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      seed = static_cast<uint32_t>(p * 7 + 3);
      for (int i = 0; i < kPerProducer; ++i) {
        bool done = false;
        try {
          T item(int64_t{p} * kPerProducer + i);  // allocates nothing
          FailScope fail(FailMode::kRandom);
          done = q.Push(std::move(item));
        } catch (const std::bad_alloc&) {
        }
        if (done) {
          ok[static_cast<size_t>(p)][static_cast<size_t>(i)] = 1;
          ++pushed;
        }
      }
    });
  }
  for (auto& t : producers) t.join();
  EXPECT_TRUE(Eventually([&] { return got.load() >= pushed.load(); }, 60s));
  q.Abort();
  for (auto& t : consumers) t.join();
  EXPECT_EQ(got.load(), pushed.load());
  EXPECT_EQ(errors.load(), 0);
  for (int p = 0; p < kProducers; ++p) {
    for (int i = 0; i < kPerProducer; ++i) {
      ASSERT_EQ(seen[static_cast<size_t>(p) * kPerProducer + i].load(), ok[p][i]) << p << "/" << i;
    }
  }
}

TEST(BoundedBlockingQueueAllocTest, RandomFailedBlocksMpmc) {
  for (size_t capacity : {BoundedBlockingQueue<int64_t>::kUnbounded, size_t{1}, size_t{100}}) {
    RandomFailuresMpmc<int64_t>(capacity);
    RandomFailuresMpmc<LockedInt>(capacity);
  }
}

}  // namespace bounded_blocking_queue_alloc_test
