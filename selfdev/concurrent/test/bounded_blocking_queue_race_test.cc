// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Races that decide whether BoundedBlockingQueue loses a wakeup it skipped
// (see Wake() and SkipPopWake()). Its own executable, which defines
// IBMW_SELFDEV_BBQ_RACE_POINT: a test thread with a role can be parked at
// a point until the test releases it, so that the threads interleave as
// the test needs; other hits can be delayed at random, as preemption would.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace bbq_race {

enum Point { kCounted, kSpun, kTaken, kCovered, kSkipped, kTimedWoken, kSignal, kAsleep, kAsleepTimed, kPoints };

// Delays one hit in `every` at a point, by 1 to `max_us` microseconds;
// never if `every` is 0.
struct Delay {
  unsigned every;
  unsigned max_us;
};
inline Delay delays[kPoints] = {};

constexpr int kRoles = 6;
constexpr auto kParkLimit = std::chrono::seconds(5);
// Once armed, the next hit of a point by the thread with that role waits
// there until released.
struct Park {
  std::atomic<bool> armed{false};
  std::atomic<bool> arrived{false};
  std::atomic<bool> released{false};
};
inline Park parks[kRoles][kPoints];
// Hits of each point by each role (0: threads without one).
inline std::atomic<unsigned> hits[kRoles][kPoints];
inline thread_local int role = 0;  // 0: none

// kAsleep, kAsleepTimed and kTimedWoken are hit under the queue's wait_mu_:
// a test must not park a thread there (it would hold up the others).
inline void At(Point p) {
  hits[role][p].fetch_add(1, std::memory_order_relaxed);
  Park& park = parks[role][p];
  if (role != 0 && park.armed.exchange(false)) {
    park.arrived = true;
    const auto end = std::chrono::steady_clock::now() + kParkLimit;
    while (!park.released && std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
    return;
  }
  const Delay d = delays[p];
  if (d.every == 0) return;
  thread_local std::minstd_rand rng(std::random_device{}());
  if (rng() % d.every != 0) return;
  std::this_thread::sleep_for(std::chrono::microseconds(1 + rng() % d.max_us));
}

}  // namespace bbq_race

#define IBMW_SELFDEV_BBQ_RACE_POINT(name) ::bbq_race::At(::bbq_race::name)

#include "ibmw_selfdev/concurrent/bounded_blocking_queue.h"

namespace bounded_blocking_queue_race_test {

using namespace std::chrono_literals;
using bbq_race::Delay;
using bbq_race::Point;
using ibmw::selfdev::concurrent::BoundedBlockingQueue;

template <class Done>
bool Eventually(Done done, std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!done()) {
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(20us);
  }
  return true;
}

class BoundedBlockingQueueRaceTest : public ::testing::Test {
 protected:
  // Roles of the threads in the scripted trials.
  enum Role { kNone, kFirst, kTimed, kSpinner, kOther, kPusher };

  void TearDown() override {
    for (Delay& d : bbq_race::delays) d = {};
    ResetParks();
  }

  static void ResetParks() {
    for (auto& of_role : bbq_race::parks) {
      for (bbq_race::Park& p : of_role) {
        p.armed = false;
        p.arrived = false;
        p.released = false;
      }
    }
  }
  static void Arm(Role r, Point p) { bbq_race::parks[r][p].armed = true; }
  static bool Arrived(Role r, Point p, std::chrono::milliseconds limit = 1s) {
    return Eventually([&] { return bbq_race::parks[r][p].arrived.load(); }, limit);
  }
  static void Release(Role r, Point p) { bbq_race::parks[r][p].released = true; }
  static void ReleaseAll() {
    for (auto& of_role : bbq_race::parks) {
      for (bbq_race::Park& p : of_role) p.released = true;
    }
  }

  static void PopOnce(BoundedBlockingQueue<int>& q, std::atomic<int>& got, std::atomic<int>* value) {
    int v = -1;
    if (q.Pop(v)) {
      if (value != nullptr) *value = v;
      ++got;
    }
  }

  // A consumer that pops once, with role `r`; stores the item in `value`.
  static std::thread Consumer(BoundedBlockingQueue<int>& q, std::atomic<int>& got, Role r,
                              std::atomic<int>* value = nullptr) {
    return std::thread([&q, &got, r, value] {
      bbq_race::role = r;
      PopOnce(q, got, value);
    });
  }

  // As Consumer(), started now but popping only once `go` is set: a
  // thread started late in a trial may first run a CFS slice later.
  static std::thread Gated(BoundedBlockingQueue<int>& q, std::atomic<int>& got, Role r, std::atomic<bool>& go,
                           std::atomic<int>* value = nullptr) {
    return std::thread([&q, &got, r, &go, value] {
      bbq_race::role = r;
      while (!go) std::this_thread::sleep_for(20us);
      PopOnce(q, got, value);
    });
  }

  // One scripted trial. kFirst counts as spinning (parked at kSpun) while
  // kTimed goes to sleep, so kTimed sleeps timed; then kFirst sleeps, not
  // timed (nothing spins then), after kTimed. kSpinner then spins (parked
  // at kSpun), so the wakes of two pushes are skipped. A third push, past
  // kSkipGrace, wakes kTimed, the oldest sleeper, `third_at` after it
  // started (or soon if zero). Once kTimed took an item, kSpinner takes the
  // other one: no skip is outstanding then, yet the third item is queued
  // and its wake was taken by kTimed. Returns whether it went so (kTimed
  // took an item before kSpinner); fails if an item was left queued.
  static bool StolenWakeTrial(std::chrono::microseconds third_at) {
    ResetParks();
    BoundedBlockingQueue<int> q;
    std::atomic<int> got{0};
    std::vector<std::thread> consumers;
    Arm(kFirst, bbq_race::kSpun);
    consumers.push_back(Consumer(q, got, kFirst));
    bool scripted = Arrived(kFirst, bbq_race::kSpun);
    Arm(kTimed, bbq_race::kTaken);
    const auto timed_start = std::chrono::steady_clock::now();
    consumers.push_back(Consumer(q, got, kTimed));
    std::this_thread::sleep_for(1ms);
    Release(kFirst, bbq_race::kSpun);
    std::this_thread::sleep_for(1ms);
    Arm(kSpinner, bbq_race::kSpun);
    consumers.push_back(Consumer(q, got, kSpinner));
    scripted = Arrived(kSpinner, bbq_race::kSpun) && scripted;
    EXPECT_TRUE(q.Push(0));
    EXPECT_TRUE(q.Push(1));
    if (third_at == third_at.zero()) {
      std::this_thread::sleep_for(200us);
    } else {
      std::this_thread::sleep_until(timed_start + third_at);
    }
    EXPECT_TRUE(q.Push(2));
    scripted = Arrived(kTimed, bbq_race::kTaken, 100ms) && scripted;
    Release(kSpinner, bbq_race::kSpun);
    scripted = Eventually([&] { return got.load() >= 1; }, 1s) && scripted;
    Release(kTimed, bbq_race::kTaken);
    const bool all = Eventually([&] { return got.load() == 3; }, 1s);
    EXPECT_TRUE(all) << 3 - got.load() << " items left queued" << (scripted ? "" : " (not as scripted)");
    ReleaseAll();
    q.Abort();  // releases the Pop calls that missed their wakeup
    for (auto& t : consumers) t.join();
    return scripted;
  }

  static unsigned Hits(int r, Point p) { return bbq_race::hits[r][p].load(std::memory_order_relaxed); }
  // Until role `r` hit `p` more than `before` times.
  static bool Hit(int r, Point p, unsigned before, std::chrono::milliseconds limit = 100ms) {
    return Eventually([&] { return Hits(r, p) > before; }, limit);
  }
  // Starts a consumer with role `r` and waits until it is asleep (inside
  // wait(): it hit `p` under wait_mu_), timed or not as `p` says.
  static bool StartAsleep(std::vector<std::thread>& consumers, BoundedBlockingQueue<int>& q, std::atomic<int>& got,
                          Role r, Point p, std::atomic<int>* value = nullptr) {
    const unsigned before = Hits(r, p);
    consumers.push_back(Consumer(q, got, r, value));
    return Hit(r, p, before);
  }
  // Ends a trial: lets every thread go and joins them.
  static void Finish(BoundedBlockingQueue<int>& q, std::vector<std::thread>& threads) {
    ReleaseAll();
    q.Abort();  // releases the Pop calls that missed their wakeup
    for (auto& t : threads) t.join();
  }
  // Runs `trial` (which returns whether it went as scripted) until
  // `wanted` trials went so, at most `limit` times; fails if too few did.
  template <class Trial>
  void RunScripted(Trial trial, int wanted, int limit) {
    int scripted = 0;
    int n = 0;
    for (; n < limit && scripted < wanted && !HasFailure(); ++n) scripted += trial();
    if (!HasFailure()) {
      EXPECT_GE(scripted, wanted / 4) << "only so many of " << n << " trials went as scripted";
    }
  }

  // One scripted trial. kFirst sleeps first (nothing spins then, so not
  // timed); kTimed sleeps timed while kOther counts as spinning (parked at
  // kSpun), then kOther sleeps, not timed. kSpinner (started at the outset,
  // see Gated()) then counts as spinning (parked at kCounted), so the wake
  // of item 0 is skipped. Items 1 and 2, past kSkipGrace, wake two
  // sleepers, on glibc the oldest, kFirst and kTimed; the trial only
  // counts if both took an item. If kFirst took item 0 and kTimed item 1,
  // kTimed took the item its signal was for and passes nothing on, but
  // kFirst, signalled although not timed, took the skipped one: it must
  // pass the wake on, to kOther, for item 2. Returns whether it went so;
  // fails if an item was left queued.
  static bool UntimedTakesSkippedTrial() {
    ResetParks();
    BoundedBlockingQueue<int> q;
    std::atomic<int> got{0};
    std::atomic<int> timed_value{-1};
    std::vector<std::thread> consumers;
    std::atomic<bool> go{false};
    const auto finish = [&] {
      go = true;
      Finish(q, consumers);
      return false;
    };
    Arm(kSpinner, bbq_race::kCounted);
    consumers.push_back(Gated(q, got, kSpinner, go));
    if (!StartAsleep(consumers, q, got, kFirst, bbq_race::kAsleep)) return finish();
    Arm(kOther, bbq_race::kSpun);
    consumers.push_back(Consumer(q, got, kOther));
    if (!Arrived(kOther, bbq_race::kSpun, 100ms)) return finish();
    if (!StartAsleep(consumers, q, got, kTimed, bbq_race::kAsleepTimed, &timed_value)) return finish();
    const auto timed_start = std::chrono::steady_clock::now();
    const unsigned other_asleep = Hits(kOther, bbq_race::kAsleep);
    Release(kOther, bbq_race::kSpun);
    if (!Hit(kOther, bbq_race::kAsleep, other_asleep)) return finish();
    go = true;
    if (!Arrived(kSpinner, bbq_race::kCounted, 100ms)) return finish();
    Arm(kFirst, bbq_race::kTaken);
    Arm(kTimed, bbq_race::kTaken);
    const unsigned skipped = Hits(kNone, bbq_race::kSkipped);
    const unsigned signals = Hits(kNone, bbq_race::kSignal);
    EXPECT_TRUE(q.Push(0));
    std::this_thread::sleep_for(200us);
    EXPECT_TRUE(q.Push(1));
    EXPECT_TRUE(q.Push(2));
    // Item 0 skipped, items 1 and 2 signalled, well within kTimed's kBackstop.
    bool scripted = Hits(kNone, bbq_race::kSkipped) == skipped + 1 && Hits(kNone, bbq_race::kSignal) == signals + 2 &&
                    std::chrono::steady_clock::now() - timed_start < 5ms;
    scripted = Arrived(kFirst, bbq_race::kTaken, 100ms) && Arrived(kTimed, bbq_race::kTaken, 100ms) && scripted;
    if (!scripted) return finish();
    const unsigned timed_covered = Hits(kTimed, bbq_race::kCovered);
    const unsigned timed_signals = Hits(kTimed, bbq_race::kSignal);
    Release(kTimed, bbq_race::kTaken);
    // kTimed took item 1 and passed nothing on, so kOther still sleeps.
    scripted = Eventually([&] { return got.load() >= 1; }, 1s) && timed_value == 1 &&
               Hits(kTimed, bbq_race::kCovered) == timed_covered && Hits(kTimed, bbq_race::kSignal) == timed_signals &&
               got.load() == 1;
    Release(kFirst, bbq_race::kTaken);
    const bool all = Eventually([&] { return got.load() == 3; }, 1s);
    EXPECT_TRUE(all) << 3 - got.load() << " items left queued" << (scripted ? "" : " (not as scripted)");
    Finish(q, consumers);
    return scripted;
  }

  // One scripted trial. kFirst counts as spinning (parked at kSpun) while
  // kTimed goes to sleep, so kTimed sleeps timed; kSpinner counts as
  // spinning too (parked at kCounted). The wakes of items 0 and 1 are
  // skipped. kPusher pushes item 2 and stops after it saw both counts
  // (kCovered). kFirst takes item 0 and stops (kTaken) until kSpinner took
  // item 1, so neither sees an outstanding skip; both leave
  // pop_spinners_. Then kPusher records its skip: re-reading the counts it
  // must wake kTimed, which would otherwise sleep up to kBackstop with
  // item 2 queued. Returns whether it went so; fails if kPusher did not
  // signal or an item was left queued.
  static bool PreemptedPusherTrial() {
    ResetParks();
    BoundedBlockingQueue<int> q;
    std::atomic<int> got{0};
    std::atomic<int> first_value{-1};
    std::atomic<int> spinner_value{-1};
    std::vector<std::thread> threads;
    std::atomic<bool> go{false};
    std::atomic<bool> go_push{false};
    std::atomic<bool> pushed{false};
    const auto finish = [&] {
      go = true;
      go_push = true;
      Finish(q, threads);
      return false;
    };
    Arm(kSpinner, bbq_race::kCounted);
    threads.push_back(Gated(q, got, kSpinner, go, &spinner_value));
    threads.emplace_back([&q, &go_push, &pushed] {
      bbq_race::role = kPusher;
      while (!go_push) std::this_thread::sleep_for(20us);
      q.Push(2);  // false once a trial that went otherwise aborted the queue
      pushed = true;
    });
    Arm(kFirst, bbq_race::kSpun);
    threads.push_back(Consumer(q, got, kFirst, &first_value));
    if (!Arrived(kFirst, bbq_race::kSpun, 100ms)) return finish();
    if (!StartAsleep(threads, q, got, kTimed, bbq_race::kAsleepTimed)) return finish();
    go = true;
    if (!Arrived(kSpinner, bbq_race::kCounted, 100ms)) return finish();
    const unsigned skipped = Hits(kNone, bbq_race::kSkipped);
    EXPECT_TRUE(q.Push(0));
    EXPECT_TRUE(q.Push(1));
    if (Hits(kNone, bbq_race::kSkipped) != skipped + 2) return finish();
    Arm(kPusher, bbq_race::kCovered);
    Arm(kPusher, bbq_race::kSignal);
    go_push = true;
    // kTimed still counted as timed when kPusher read the counts.
    if (!Arrived(kPusher, bbq_race::kCovered, 100ms)) return finish();
    Arm(kFirst, bbq_race::kTaken);
    Release(kFirst, bbq_race::kSpun);
    if (!Arrived(kFirst, bbq_race::kTaken, 100ms)) return finish();
    Release(kSpinner, bbq_race::kCounted);
    if (!Eventually([&] { return got.load() >= 1; }, 100ms)) return finish();
    Release(kFirst, bbq_race::kTaken);
    if (!Eventually([&] { return got.load() >= 2; }, 100ms)) return finish();
    if (first_value != 0 || spinner_value != 1 || got.load() != 2) return finish();
    const unsigned pusher_skipped = Hits(kPusher, bbq_race::kSkipped);
    Release(kPusher, bbq_race::kCovered);
    // Signalled (parked at kSignal), or returned from Push without.
    Eventually([&] { return Arrived(kPusher, bbq_race::kSignal, 0ms) || pushed.load(); }, 1s);
    const bool skip = Hits(kPusher, bbq_race::kSkipped) > pusher_skipped;
    const bool returned = pushed;  // not yet if kPusher stalled on its way
    const bool signalled = bbq_race::parks[kPusher][bbq_race::kSignal].arrived;
    ReleaseAll();
    const bool all = Eventually([&] { return got.load() == 3; }, 1s);
    EXPECT_TRUE(all) << 3 - got.load() << " items left queued";
    if (skip && returned) {
      EXPECT_TRUE(signalled) << "the push left item 2 to kTimed's kBackstop";
    }
    Finish(q, threads);
    return skip && (returned || signalled);
  }

  // As BQ-42: each consumer pops once and leaves, so no other Pop would
  // take an item whose wakeup was lost.
  static void AllItemsTaken(int trials) {
    constexpr int kConsumers = 4;
    std::mt19937 rng(42);
    for (int trial = 0; trial < trials; ++trial) {
      BoundedBlockingQueue<int> q;
      std::atomic<int> got{0};
      std::vector<std::thread> consumers;
      for (int c = 0; c < kConsumers; ++c) consumers.push_back(Consumer(q, got, kNone));
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 300));
      for (int i = 0; i < kConsumers; ++i) {
        EXPECT_TRUE(q.Push(i));
        if (rng() % 2 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 30));
      }
      const bool all = Eventually([&] { return got.load() == kConsumers; }, 2s);
      EXPECT_TRUE(all) << "trial " << trial << ": " << kConsumers - got.load() << " items left queued";
      q.Abort();
      for (auto& t : consumers) t.join();
      if (!all) return;
    }
  }
};

// RQ-1: a timed sleeper a skip counted on, signalled for another item,
// passes the wake on after it took one, also when no skip is outstanding
// then.
TEST_F(BoundedBlockingQueueRaceTest, SignalledTimedSleeperPassesWakeOn) {
  RunScripted([] { return StolenWakeTrial(0us); }, 50, 200);
}

// RQ-2: as RQ-1, with the signal near the end of the timed sleep (the
// queue's kBackstop, 10 ms): a sleeper signalled may still find its wait
// timed out (wait_until() judges by the clock after the wait).
TEST_F(BoundedBlockingQueueRaceTest, SignalAtBackstopEnd) {
  std::mt19937 rng(42);
  RunScripted([&rng] { return StolenWakeTrial(std::chrono::microseconds(9700 + rng() % 400)); }, 200, 400);
}

// RQ-3: no item is left queued with consumers and pushers delayed now
// and then at the points outside wait_mu_, and at kTimedWoken.
TEST_F(BoundedBlockingQueueRaceTest, DelaysAtAllRacePoints) {
  bbq_race::delays[bbq_race::kCounted] = {2, 300};
  bbq_race::delays[bbq_race::kSpun] = {2, 2000};
  bbq_race::delays[bbq_race::kTaken] = {2, 1000};
  bbq_race::delays[bbq_race::kCovered] = {2, 300};
  bbq_race::delays[bbq_race::kSkipped] = {2, 300};
  bbq_race::delays[bbq_race::kTimedWoken] = {4, 3000};
  bbq_race::delays[bbq_race::kSignal] = {2, 300};
  AllItemsTaken(300);
}

// RQ-4: an untimed sleeper that was signalled passes the wake on after it
// took an item whose wake was skipped: the signal may have been for
// another item.
TEST_F(BoundedBlockingQueueRaceTest, SignalledUntimedSleeperPassesWakeOn) {
  RunScripted(UntimedTakesSkippedTrial, 20, 200);
}

// RQ-5: a pusher preempted after it saw a spinner and a timed sleeper
// counted, while both took other items without seeing its skip, wakes the
// timed sleeper: it re-reads the counts after recording the skip.
TEST_F(BoundedBlockingQueueRaceTest, PreemptedPusherRereadsCounts) {
  RunScripted(PreemptedPusherTrial, 20, 200);
}

}  // namespace bounded_blocking_queue_race_test
