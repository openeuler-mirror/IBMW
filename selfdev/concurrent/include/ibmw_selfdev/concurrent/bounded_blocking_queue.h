// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <ratio>
#include <thread>
#include <type_traits>
#include <utility>

#include "ibmw_selfdev/concurrent/detail/adaptive_lock.h"
#include "ibmw_selfdev/concurrent/detail/asymmetric_barrier.h"

// A test executable may define this before including the header (in every
// file of it that does), to delay threads at points where races decide
// whether a wakeup is lost (see test/bounded_blocking_queue_race_test.cc).
// Does nothing otherwise.
#ifndef IBMW_SELFDEV_BBQ_RACE_POINT
#define IBMW_SELFDEV_BBQ_RACE_POINT(name) static_cast<void>(0)
#endif

namespace ibmw::selfdev::concurrent {

namespace detail {
#if defined(__SANITIZE_THREAD__)
inline constexpr bool kThreadSanitizer = true;
#elif defined(__has_feature)
  #if __has_feature(thread_sanitizer)
inline constexpr bool kThreadSanitizer = true;
  #else
inline constexpr bool kThreadSanitizer = false;
  #endif
#else
inline constexpr bool kThreadSanitizer = false;
#endif

// One wait's view of whether this thread gets little CPU, kept in a flag of
// a table that threads share (see BoundedBlockingQueue::slow_). A flag
// holds the mark of the thread that set it, or 0, so that a thread clears
// only its own.
template <int kBits>
class SlowFlag {
 public:
  static_assert(kBits > 0 && kBits <= 16, "a flag index and a 7-bit mark from one hash");
  using Flags = std::atomic<uint8_t>[1 << kBits];

  // Thread ids are often addresses of thread structures a fixed stride
  // apart (0x103000 on the OHOS board), so mixed by murmur3's finalizer:
  // Fibonacci hashing put three such threads in two neighbouring flags of
  // 64 there.
  static uint64_t Mix(uint64_t id) {
    id = (id ^ (id >> 33)) * 0xFF51AFD7ED558CCDu;
    id = (id ^ (id >> 33)) * 0xC4CEB9FE1A85EC53u;
    return id ^ (id >> 33);
  }
  static uint64_t ThreadHash() {
    return Mix(static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id())));
  }

  // Picks the flag by the top bits of `hash`, the mark by the next 7.
  // Until bound, Get() is false and Set() does nothing.
  void Bind(Flags& flags, uint64_t hash = ThreadHash()) {
    flag_ = &flags[hash >> (64 - kBits)];
    mark_ = static_cast<uint8_t>(0x80 | ((hash >> (64 - kBits - 7)) & 0x7F));
    slow_ = flag_->load(std::memory_order_relaxed) == mark_;
  }
  bool Get() const { return slow_; }
  void Set(bool slow) {
    if (flag_ == nullptr || slow == slow_) return;
    slow_ = slow;
    if (slow) {
      flag_->store(mark_, std::memory_order_relaxed);
    } else {
      uint8_t mine = mark_;  // another thread's mark stays
      flag_->compare_exchange_strong(mine, 0, std::memory_order_relaxed);
    }
  }

 private:
  std::atomic<uint8_t>* flag_ = nullptr;
  uint8_t mark_ = 0;
  bool slow_ = false;
};
}  // namespace detail

// See IBMW_SELFDEV_BARRIER_NS in detail/asymmetric_barrier.h.
inline namespace IBMW_SELFDEV_BARRIER_NS {

/// Multi-producer multi-consumer FIFO queue with optional capacity bound.
/// Replaces tbb::concurrent_bounded_queue. Unlike TBB, Abort() does not throw
/// in waiting threads: blocking calls return false, and Abort() is sticky
/// until Reset().
///
/// Every item gets a ticket from one atomic counter and lives in lane
/// `ticket % kLanes`, so concurrent producers write to different memory and
/// take no lock. Consumers take tickets from a second counter, in order, and
/// also take no lock when T moves without throwing; otherwise they take
/// items under one short lock, so that a throwing move leaves the item in
/// the queue. A thread that has to wait for another one (queue empty or
/// full, the previous producer or consumer of a lane not done, or an item
/// still being written) spins briefly and then sleeps on a condition
/// variable; wakers only signal when a sleeper is registered.
///
/// A push may leave sleeping consumers asleep while another consumer is
/// spinning for an item (see Wake()), unless an item such a push left has
/// been queued for kSkipGrace (50 us). If the spinning consumers then do not
/// get the CPU (preempted, or at low priority), the item waits in the queue
/// until the next push, or until a sleeper re-checks after at most kBackstop
/// (10 ms) if that sleeper gets the CPU; once such an item has waited
/// kHeldBack (1 ms), pushes wake sleepers for kSkipOff (1 s). A consumer
/// thread that finds it gets little CPU keeps yielding instead of sleeping
/// (kMinYields), so that wakes go to consumers that run. As with tbb, a
/// consumer woken for an item that then does not get the CPU delays that
/// item until it runs.
///
/// T's constructors, move operations and destructor run while the calling
/// thread holds its place in a lane, so they must not use the same queue.
/// Memory: kLanes blocks of kBlockSlots cells (8 x 16 x sizeof(T), plus a
/// state byte per cell) are allocated up front, and each lane keeps at most
/// one drained block for reuse.
template <class T>
class BoundedBlockingQueue {
 public:
  static constexpr size_t kUnbounded = std::numeric_limits<size_t>::max();

  /// A capacity of 0 is treated as 1.
  explicit BoundedBlockingQueue(size_t capacity = kUnbounded) : capacity_(Normalize(capacity)) {
    detail::RegisterMembarrier();
    size_t made = 0;
    try {
      for (; made < kLanes; ++made) {
        Block* b = new Block;
        lanes_[made].tail_block = b;
        lanes_[made].head_block = b;
      }
    } catch (...) {
      for (size_t k = 0; k < made; ++k) delete lanes_[k].head_block;
      throw;
    }
  }

  BoundedBlockingQueue(const BoundedBlockingQueue&) = delete;
  BoundedBlockingQueue& operator=(const BoundedBlockingQueue&) = delete;

  /// No thread may use the queue while it is destroyed.
  ~BoundedBlockingQueue() {
    size_t passed = 0;
    while (TakeFront([](T&) noexcept {}, passed, nullptr) == Front::kItem) {
    }
    for (Lane& ln : lanes_) {
      for (Block* b = ln.head_block; b != nullptr;) delete std::exchange(b, b->next.load(std::memory_order_relaxed));
      delete ln.spare.load(std::memory_order_relaxed);
    }
  }

  /// Blocks while the queue is full. Returns false (value untouched) once
  /// aborted, also when Reset() follows the Abort() before this call wakes.
  bool Push(const T& value) { return Emplace(value); }
  bool Push(T&& value) { return Emplace(std::move(value)); }

  template <class... Args>
  bool Emplace(Args&&... args) {
    const uint64_t epoch = abort_.load(std::memory_order_seq_cst);
    while (true) {
      switch (TryEmplaceImpl(std::forward<Args>(args)...)) {
        case PushResult::kPushed:
          return true;
        case PushResult::kAborted:
          return false;
        case PushResult::kFull:
          break;
      }
      if (Wait(kPushSleeper, [this] { return Full(); }, epoch, kNoDeadline) != Woken::kRetry) {
        return false;
      }
    }
  }

  /// Returns false when the queue is full or aborted; the value is untouched.
  /// A push whose constructor threw keeps its slot until a consumer steps
  /// over it.
  bool TryPush(const T& value) { return TryEmplace(value); }
  bool TryPush(T&& value) { return TryEmplace(std::move(value)); }

  template <class... Args>
  bool TryEmplace(Args&&... args) {
    return TryEmplaceImpl(std::forward<Args>(args)...) == PushResult::kPushed;
  }

  /// Blocks until an item can be taken. Returns false (out untouched) once
  /// aborted, also when Reset() follows the Abort() before this call wakes.
  bool Pop(T& out) { return PopUntil(out, kNoDeadline); }

  /// Like Pop() but gives up at `timeout`; every wait inside, including one
  /// for an item still being written, ends by then. duration::max() (or any
  /// timeout past the clock's range) waits without limit.
  template <class Rep, class Period>
  bool PopFor(T& out, std::chrono::duration<Rep, Period> timeout) {
    return PopUntil(out, DeadlineAfter(timeout));
  }

  /// Returns false when the queue is empty. Still works after Abort() so
  /// remaining items can be drained.
  ///
  /// The oldest item may not be ready yet: its producer is still writing it
  /// (that Push has not returned), or the consumer of the item kLanes places
  /// ahead of it is still moving that one out. When T moves without
  /// throwing, TryPop() then takes the oldest ticket anyway and waits for
  /// that thread (briefly spinning, then sleeping; also after Abort(), as
  /// the ticket is already its own), which is usually a few nanoseconds.
  /// Otherwise it returns false without waiting; items behind the oldest
  /// stay queued (FIFO by ticket), so Size() may be non-zero then.
  bool TryPop(T& out) { return TryPopImpl(out, nullptr) == Front::kItem; }

  /// Wakes every blocked Push/Pop (they return false). Later blocking calls
  /// return false immediately until Reset().
  void Abort() {
    for (uint64_t s = abort_.load(std::memory_order_seq_cst);
         (s & 1) == 0 && !abort_.compare_exchange_weak(s, s + 1, std::memory_order_seq_cst);) {
    }
    { std::lock_guard<std::mutex> lock(wait_mu_); }
    not_empty_.notify_all();
    not_full_.notify_all();
    progress_.notify_all();
  }

  /// Clears the aborted state; queued items are kept. Calls that were
  /// blocked when Abort() came still return false.
  void Reset() {
    for (uint64_t s = abort_.load(std::memory_order_seq_cst);
         (s & 1) != 0 && !abort_.compare_exchange_weak(s, s + 1, std::memory_order_seq_cst);) {
    }
  }

  bool Aborted() const { return (abort_.load(std::memory_order_seq_cst) & 1) != 0; }

  /// A capacity of 0 is treated as 1. Growing wakes blocked producers.
  void SetCapacity(size_t capacity) {
    capacity_.store(Normalize(capacity), std::memory_order_seq_cst);
    { std::lock_guard<std::mutex> lock(wait_mu_); }
    not_full_.notify_all();
  }

  size_t Capacity() const { return capacity_.load(std::memory_order_seq_cst); }

  /// Includes items whose producer is still writing them, not items a
  /// consumer is taking out.
  size_t Size() const {
    // popped first, tail last, so pushed - popped never wraps. A consumer
    // uncounts a skipped ticket just after passing it, so live - skipped
    // can briefly be negative: clamped.
    const uint64_t popped = head_.popped.load(std::memory_order_seq_cst);
    const uint64_t skipped = skipped_.load(std::memory_order_seq_cst);
    const uint64_t pushed = tail_.load(std::memory_order_seq_cst);
    const uint64_t live = pushed - popped;
    return live > skipped ? static_cast<size_t>(live - skipped) : 0;
  }

  bool Empty() const { return Size() == 0; }

  /// Drops all items (waiting for pushes already in progress to finish
  /// writing theirs) and wakes blocked producers.
  void Clear() {
    const uint64_t target = tail_.load(std::memory_order_seq_cst);
    while (head_.popped.load(std::memory_order_seq_cst) < target) {
      size_t passed = 0;
      const Front front = TakeFront([](T&) noexcept {}, passed, nullptr);
      if (passed != 0) OnPopped(passed);
      if (front == Front::kNotReady) {
        WaitFront(head_.popped.load(std::memory_order_seq_cst), kIgnoreAbort, kNoDeadline);
      }
    }
    { std::lock_guard<std::mutex> lock(wait_mu_); }
    not_full_.notify_all();
  }

 private:
  using Clock = std::chrono::steady_clock;
  static constexpr Clock::time_point kNoDeadline = Clock::time_point::max();
  // Ticket t goes to lane t % kLanes, at lane index t / kLanes. Eight lanes
  // let eight producers write at once without sharing cache lines.
  static constexpr size_t kLanes = 8;
  // Cells per block; a lane allocates one block per kBlockSlots of its items
  // at most, and recycles drained blocks through Lane::spare.
  static constexpr size_t kBlockSlots = 16;
  // Before sleeping, a waiting thread (in Pop, or in Push on a full queue)
  // pause-spins, then yields. Bounded, so a preempted thread it waits for
  // (even on the same CPU under SCHED_FIFO) soon gets the CPU back. While a
  // consumer sleeps, each push pays a futex wake: on the OHOS board (HongMeng
  // kernel) that was most of a producer's time with 1 producer and 8
  // consumers. Up to kWaitYields yields, and mostly no wake while a
  // consumer spins (see Wake()), made 1P8C 2 to 3 times as fast as a fixed 4
  // yields. But a yield there costs about 3 us of CPU, wasted when no item
  // comes: the number of yields adapts (see YieldBudget), so that consumers
  // fed every 20 or 200 us use about as much CPU as tbb's, which sleep at
  // once (the sparse cases in queue_bench).
  static constexpr int kWaitSpins = 64;
  static constexpr int kWaitYields = 16;
  // Fewest yields a pop waiter makes on a thread that gets little CPU
  // (preempted, or at low priority): one whose last yield took over
  // kYieldStall, or which ran over kWakeStall after the last pop wake when
  // it woke from a sleep. It takes long over them, so it is mostly yielding
  // rather than asleep, and wakes go to sleepers that run; with no yields it
  // would sleep whenever it got the CPU and take wakes it cannot act on soon.
  // Per thread and queue (see slow_), not a floor of the shared budget: that
  // cost the sparse cases in queue_bench 10 to 25% more CPU on the OHOS board.
  static constexpr int kMinYields = 4;
  static constexpr Clock::duration kYieldStall = std::chrono::microseconds(50);
  static constexpr Clock::duration kWakeStall = std::chrono::milliseconds(1);
  // log2 of the number of flags in slow_.
  static constexpr int kSlowFlagBits = 6;
  // How long a pop sleeper sleeps at most while a pop waiter spins, so that
  // an item whose wake was skipped is taken that soon even if no spinner
  // gets the CPU (see Wake()).
  static constexpr Clock::duration kBackstop = std::chrono::milliseconds(10);
  // How long after a skipped wake pushes may still skip theirs while its
  // item is queued, for spinners that are about to take it (see SkipGate).
  // Skipping only while no such item was queued made consumers fed every
  // 20 us use about 1.5 times as much CPU on the OHOS board (the sparse
  // cases in queue_bench).
  static constexpr Clock::duration kSkipGrace = std::chrono::microseconds(50);
  // How long after its skipped wake an item in the queue counts as held back,
  // not just about to be taken by a spinner that runs (see SkipGate).
  static constexpr Clock::duration kHeldBack = std::chrono::milliseconds(1);
  // How long no wake is skipped after a skipped one held an item back (see
  // SkipGate); meanwhile pop waiters yield only as kMinYields says (see
  // Wait()).
  static constexpr Clock::duration kSkipOff = std::chrono::seconds(1);
  // A front wait is for a thread in the middle of a push or pop, which is
  // nearly always running (e.g. allocating a block) and done within these
  // yields. Sleeping there instead would cost a HeavyBarrier() and make
  // every push and pop take WakeSlow() while it sleeps, slowing the thread
  // waited for: with 8 producers that fed on itself.
  static constexpr int kFrontYields = 64;
  // Re-check period of a front sleeper whose HeavyBarrier() failed.
  static constexpr Clock::duration kUnpairedPoll = std::chrono::milliseconds(1);
  // abort_ never reaches this value; for waits that ignore Abort().
  static constexpr uint64_t kIgnoreAbort = std::numeric_limits<uint64_t>::max();
  // Units of sleepers_, one 21-bit count per kind, so that a waker reads
  // one word: Pop() on an empty queue, Push() on a full one, and any wait
  // for another thread at a lane (front).
  static constexpr uint64_t kPopSleeper = 1;
  static constexpr uint64_t kPushSleeper = uint64_t{1} << 21;
  static constexpr uint64_t kFrontSleeper = uint64_t{1} << 42;
  static constexpr uint64_t kSleeperMask = (uint64_t{1} << 21) - 1;
  // Set in Lane::head_turn while TakeHeld() holds the lane.
  static constexpr uint64_t kBusy = uint64_t{1} << 63;
  // Consumers can only give a ticket back under a lock; without one, taking
  // the item out of its cell must not throw.
  static constexpr bool kTicketedPop =
      std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>;

  enum : uint8_t { kEmpty = 0, kFull = 1, kSkipped = 2 };

  struct Cell {
    alignas(T) unsigned char storage[sizeof(T)];
    // kEmpty -> kFull (item published) or kSkipped (its push threw).
    std::atomic<uint8_t> state{kEmpty};
    T* Item() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
  };

  struct Block {
    std::atomic<Block*> next{nullptr};
    Cell cells[kBlockSlots];
  };

  struct Lane {
    // Producer side. Producers of a lane write in ticket order: the one
    // whose lane index equals `turn` owns tail_block / tail_seq. turn > i
    // means the push of lane index i is finished (item or skipped).
    alignas(64) std::atomic<uint64_t> turn{0};
    Block* tail_block = nullptr;
    uint64_t tail_seq = 0;  // block number (lane index / kBlockSlots) of tail_block
    // Consumer side: head_turn is the lane index of the next item to take;
    // the consumer that owns it (see TakeHeld()), or without kTicketedPop
    // the holder of head_.mu, uses the rest.
    alignas(64) std::atomic<uint64_t> head_turn{0};
    Block* head_block = nullptr;
    uint64_t head_seq = 0;
    // One drained block kept for reuse; exchanged by both sides.
    std::atomic<Block*> spare{nullptr};
  };

  enum class PushResult { kPushed, kFull, kAborted };
  enum class Front { kItem, kEmpty, kNotReady };
  enum class Slot { kItem, kSkipped, kNotReady };
  enum class Woken { kRetry, kAborted, kTimeout };

  // For a consumer that may wait at the front: until abort_ leaves `epoch`
  // (never with kIgnoreAbort) or the deadline passes; `woken` says why it
  // stopped waiting.
  struct Hold {
    uint64_t epoch;
    Clock::time_point deadline;
    Woken woken = Woken::kRetry;
  };

  // Yields a pop or push waiter makes before it sleeps: more while waits
  // end during them or the spins before, fewer (down to none) while they
  // do not, so that waits too long to spin through soon cost little CPU.
  // Shared by the waiters of one kind; a lost update only shifts the count
  // by one or two. Waits that end in yields beyond it (kMinYields) are no
  // hits: else waiters that get little CPU, as all do on a loaded machine,
  // keep every waiter yielding.
  class YieldBudget {
   public:
    int Get() const { return n_.load(std::memory_order_relaxed); }
    void Hit() {
      const int n = Get();
      if (n < kWaitYields) n_.store(std::min(n + 2, kWaitYields), std::memory_order_relaxed);
    }
    void Miss() {
      const int n = Get();
      if (n > 0) n_.store(n - 1, std::memory_order_relaxed);
    }

   private:
    std::atomic<int> n_{kWaitYields};
  };

  // Whether a pop wake may be skipped for a spinning consumer: yes, unless
  // an item whose wake was skipped has been queued since a skip kSkipGrace
  // ago, so spinners are not taking items, or a skip lately held an item
  // back, i.e. its item was still queued kHeldBack after the skip when a
  // wake (Skip()) or the end of a timed pop sleeper's kBackstop (Missed())
  // found it; then not for kSkipOff. An item still queued for other reasons
  // (a producer preempted while writing an earlier one, a backlog, a loaded
  // machine) can also turn skips off: that only costs wakes. Racing skips
  // can pair one's end_ with the other's at_; that only makes one
  // misjudged.
  class SkipGate {
   public:
    // Records a skip of the wake for an item pushed while `popped` items
    // were taken and `tail` tickets handed out; false while skips are off
    // or an item of an earlier skip has been queued kSkipGrace. The caller
    // may wake after all (see Wake()); the item then only counts as
    // outstanding, which costs a pass-on or turns skips off sooner.
    bool Skip(uint64_t popped, uint64_t tail) {
      const Clock::rep now = Now();
      if (now < off_until_.load(std::memory_order_relaxed)) return false;
      const bool outstanding = Outstanding(popped);  // spinners have not taken it yet
      if (outstanding && now - at_.load(std::memory_order_relaxed) >= kGrace) {
        TurnOffIfHeldBack(now);
        return false;
      }
      // The latest skip's tail_, also when racing skips store out of order;
      // also within the grace, so that this item counts as outstanding too.
      // seq_cst: see Wake().
      uint64_t end = end_.load(std::memory_order_seq_cst);
      while (end < tail && !end_.compare_exchange_weak(end, tail, std::memory_order_seq_cst)) {
      }
      if (!outstanding) at_.store(now, std::memory_order_relaxed);
      return true;
    }
    // Whether an item whose wake was skipped may be queued after `popped`
    // items were taken.
    bool Outstanding(uint64_t popped) const { return popped < end_.load(std::memory_order_seq_cst); }
    bool Off() const { return Now() < off_until_.load(std::memory_order_relaxed); }
    // Called by a pop sleeper whose kBackstop ended with `popped` items taken.
    void Missed(uint64_t popped) {
      if (Outstanding(popped)) TurnOffIfHeldBack(Now());
    }

   private:
    static Clock::rep Now() { return Clock::now().time_since_epoch().count(); }
    void TurnOffIfHeldBack(Clock::rep now) {
      if (now - at_.load(std::memory_order_relaxed) >= kHeld) off_until_.store(now + kOff, std::memory_order_relaxed);
    }
    static constexpr Clock::rep kGrace = kSkipGrace.count();
    static constexpr Clock::rep kHeld = kHeldBack.count();
    static constexpr Clock::rep kOff = kSkipOff.count();
    // tail_ at the last skip, and when the first skip made with no item of
    // an earlier one queued was.
    std::atomic<uint64_t> end_{0};
    std::atomic<Clock::rep> at_{0};
    // Clock::rep before which no wake is skipped.
    std::atomic<Clock::rep> off_until_{0};
  };

  static size_t Normalize(size_t capacity) { return capacity == 0 ? 1 : capacity; }
  static size_t Index(uint64_t lane_index) { return static_cast<size_t>(lane_index % kBlockSlots); }

  template <class... Args>
  PushResult TryEmplaceImpl(Args&&... args) {
    if ((abort_.load(std::memory_order_seq_cst) & 1) != 0) return PushResult::kAborted;
    uint64_t ticket;
    if (!Claim(ticket)) return PushResult::kFull;
    Publish(ticket, std::forward<Args>(args)...);
    return PushResult::kPushed;
  }

  // Takes the next ticket unless the queue is full. seq_cst: see Wake().
  bool Claim(uint64_t& ticket) {
    const size_t capacity = capacity_.load(std::memory_order_relaxed);
    if (capacity == kUnbounded) {
      ticket = tail_.fetch_add(1, std::memory_order_seq_cst);
      return true;
    }
    uint64_t t = tail_.load(std::memory_order_relaxed);
    while (true) {
      // A cached popped newer than t only means t is stale; the CAS fails then.
      uint64_t popped = popped_cache_.load(std::memory_order_relaxed);
      if (t >= popped && t - popped >= capacity) {
        popped = head_.popped.load(std::memory_order_acquire);
        popped_cache_.store(popped, std::memory_order_relaxed);
        if (t >= popped && t - popped >= capacity) return false;
      }
      if (tail_.compare_exchange_weak(t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
        ticket = t;
        return true;
      }
    }
  }

  // Writes the item for `ticket`. If that throws, the ticket becomes a
  // skipped slot: consumers step over it, and Size() does not count it.
  template <class... Args>
  void Publish(uint64_t ticket, Args&&... args) {
    Lane& ln = lanes_[ticket % kLanes];
    const uint64_t li = ticket / kLanes;
    if (ln.turn.load(std::memory_order_acquire) != li) {
      // The lane's previous producer is still writing: more than kLanes
      // pushes in flight, or it was preempted. The ticket is ours, so wait
      // even if aborted.
      Wait(kFrontSleeper, [&ln, li] { return ln.turn.load(std::memory_order_acquire) != li; },
           kIgnoreAbort, kNoDeadline);
    }
    // However this ends, the lane passes to its next producer and consumers
    // are woken: they wait for a skipped ticket too, which holds a capacity
    // slot until one of them steps over it.
    struct PassTurn {
      BoundedBlockingQueue& q;
      Lane& ln;
      uint64_t next;
      ~PassTurn() {
        ln.turn.store(next, std::memory_order_release);
        detail::LightBarrier();
        q.Wake(kPopSleeper, 1);
      }
    } pass{*this, ln, li + 1};
    Cell* cell;
    try {
      cell = &ProducerCell(ln, li);
    } catch (...) {
      // No block for this ticket: consumers find the block missing after
      // the turn has passed it (see Probe()).
      skipped_.fetch_add(1, std::memory_order_relaxed);
      throw;
    }
    try {
      ::new (static_cast<void*>(cell->storage)) T(std::forward<Args>(args)...);
    } catch (...) {
      skipped_.fetch_add(1, std::memory_order_relaxed);
      cell->state.store(kSkipped, std::memory_order_release);
      throw;
    }
    cell->state.store(kFull, std::memory_order_release);
  }

  // Under the lane turn. Links blocks up to the one holding lane index li;
  // may throw (allocation) with the chain still consistent.
  Cell& ProducerCell(Lane& ln, uint64_t li) {
    const uint64_t seq = li / kBlockSlots;
    while (ln.tail_seq < seq) {
      Block* b = TakeSpare(ln);
      // Normally li is the first index of the new block. Earlier indexes in
      // it belong to pushes whose block allocation failed.
      const size_t lost = ln.tail_seq + 1 < seq ? kBlockSlots : Index(li);
      for (size_t k = 0; k < lost; ++k) b->cells[k].state.store(kSkipped, std::memory_order_relaxed);
      ln.tail_block->next.store(b, std::memory_order_release);
      ln.tail_block = b;
      ++ln.tail_seq;
    }
    return ln.tail_block->cells[Index(li)];
  }

  // Front::kItem when an item was taken, else why not. With `hold` (Pop,
  // PopFor) a consumer waits at the front only until the hold's deadline or
  // an Abort(); without (TryPop, Clear) it takes the front ticket first and
  // then waits for it as long as needed.
  Front TryPopImpl(T& out, Hold* hold) {
    size_t passed = 0;
    if constexpr (kTicketedPop) {
      // Move the item out first and assign it afterwards, so whatever `out`
      // held (e.g. a callable's heap state) is destroyed after the lane is
      // handed to the next consumer and waiters are woken.
      alignas(T) unsigned char buf[sizeof(T)];
      T* tmp = nullptr;
      const Front front = TakeFront(
          [&](T& item) noexcept { tmp = ::new (static_cast<void*>(buf)) T(std::move(item)); }, passed, hold);
      if (passed != 0) OnPopped(passed);
      if (front == Front::kItem) {
        out = std::move(*tmp);
        tmp->~T();
      }
      return front;
    } else {
      // A throwing move leaves the item queued, but skipped tickets passed
      // before it are gone and their slots free.
      struct Notify {
        BoundedBlockingQueue& q;
        size_t& passed;
        ~Notify() {
          if (passed != 0) q.OnPopped(passed);
        }
      } notify{*this, passed};
      return TakeFront([&](T& item) { out = std::move(item); }, passed, hold);
    }
  }

  bool PopUntil(T& out, Clock::time_point deadline) {
    const uint64_t epoch = abort_.load(std::memory_order_seq_cst);
    if ((epoch & 1) != 0) return false;
    Hold hold{epoch, deadline};
    bool waited = false;
    bool signalled = false;  // see Wait()
    while (true) {
      Woken woken = Woken::kRetry;
      bool taken = false;
      // At most the ticket this call takes (head_.popped only grows); read
      // only once a sleep of this call may have been signalled.
      const uint64_t from = signalled ? head_.popped.load(std::memory_order_relaxed) : 0;
      try {
        switch (TryPopImpl(out, &hold)) {
          case Front::kItem:
            taken = true;
            break;
          case Front::kEmpty:
            woken = Wait(kPopSleeper, [this] { return EmptyNow(); }, epoch, deadline, &signalled);
            waited = true;
            break;
          case Front::kNotReady: {
            if (hold.woken != Woken::kRetry) return GiveUp();
            woken = WaitFront(head_.popped.load(std::memory_order_seq_cst), epoch, deadline);
            break;
          }
        }
      } catch (...) {
        GiveUp();  // a throwing move left the item queued, or a wait failed
        throw;
      }
      if (taken) {
        IBMW_SELFDEV_BBQ_RACE_POINT(kTaken);
        // A push may not wake a sleeper while a consumer spins (see
        // Wake()), so after a wait pass the signal on for the items left if
        // the wake of any of them may have been skipped. If this call was
        // signalled, also if the item it took may be one whose wake was
        // skipped (one taken from `from` on): the signal was then for
        // another item. Not for every signalled call: that cost about 10%
        // CPU in the sparse cases of the queue benchmark on the board.
        if (waited && skip_gate_.Outstanding(signalled ? from : head_.popped.load(std::memory_order_relaxed)) &&
            !EmptyNow()) {
          Wake(kPopSleeper, 1);
        }
        return true;
      }
      if (woken != Woken::kRetry) return GiveUp();
      if (abort_.load(std::memory_order_seq_cst) != epoch) return GiveUp();
    }
  }

  // A push signals one pop sleeper (see Publish() and Wake()). If that was
  // this call and it gives up (deadline, Abort() or an exception) without
  // an item, e.g. because the oldest item is still being written, the other
  // pop sleepers would sleep with items queued: pass the signal on. Like a
  // push, read sleepers_ after seeing the queue non-empty; tail_ and
  // head_.popped are seq_cst too, so a pop sleeper counted after that read
  // finds items (or a consumer took them) and does not sleep.
  bool GiveUp() {
    if (!EmptyNow()) Wake(kPopSleeper, 1);
    return false;
  }

  // Hands the oldest item to take(item) and removes it, stepping over
  // skipped tickets; `passed` counts the tickets removed and not yet
  // reported to OnPopped(). Without kTicketedPop it never waits, and a
  // throwing take leaves the item queued.
  template <class Take>
  Front TakeFront(Take&& take, size_t& passed, Hold* hold) {
    if constexpr (kTicketedPop) {
      return hold == nullptr ? TakeClaimed(take, passed) : TakeHeld(take, passed, *hold);
    } else {
      std::lock_guard<detail::AdaptiveMutex> lock(head_.mu);
      return PopFrontLocked(take, passed);
    }
  }

  // Lock-free consumers own the consumer side of lane ln for lane index li
  // (Probe() and the cell's item) in one of two ways:
  //  - TakeClaimed() first removes the ticket (head_.popped: t -> t + 1),
  //    then waits until head_turn == li;
  //  - TakeHeld() first takes the lane (head_turn: li -> li | kBusy), then
  //    re-reads head_.popped and keeps the lane only if it is still t; it
  //    removes the ticket only once the item is there, so it can let go of
  //    the lane after a deadline without having taken anything.
  // All four steps are seq_cst. If TakeHeld() re-reads t, TakeClaimed()'s
  // removal comes later, so TakeClaimed() then reads li | kBusy and waits,
  // and TakeHeld()'s own removal fails. If TakeClaimed() read li first,
  // TakeHeld() re-reads t + 1 and lets go without touching the lane. Letting
  // go is a CAS li | kBusy -> li, so it never undoes TakeClaimed()'s li + 1.
  // The owner hands the lane on with head_turn = li + 1.

  // Pop / PopFor.
  template <class Take>
  Front TakeHeld(Take& take, size_t& passed, Hold& hold) {
    uint64_t ticket = head_.popped.load(std::memory_order_seq_cst);
    while (true) {
      if (!Pending(ticket)) return Front::kEmpty;
      Lane& ln = lanes_[ticket % kLanes];
      const uint64_t li = ticket / kLanes;
      uint64_t free = li;
      if (!ln.head_turn.compare_exchange_strong(free, li | kBusy, std::memory_order_seq_cst,
                                                std::memory_order_relaxed)) {
        // Another consumer owns the lane, the lane's previous consumer is not
        // done, or `ticket` is gone. An owner is usually about to move on:
        // give it a moment, then let PopUntil() sleep in WaitFront().
        uint64_t now = head_.popped.load(std::memory_order_seq_cst);
        for (int n = 0; now == ticket && ln.head_turn.load(std::memory_order_relaxed) != li; ++n) {
          if (n == kWaitSpins) return Front::kNotReady;
          detail::CpuRelax();
          now = head_.popped.load(std::memory_order_seq_cst);
        }
        ticket = now;
        continue;
      }
      if (const uint64_t now = head_.popped.load(std::memory_order_seq_cst); now != ticket) {
        LetGo(ln, li);
        ticket = now;
        continue;
      }
      Cell* cell = nullptr;
      Slot slot;
      while ((slot = Probe(ln, li, cell)) == Slot::kNotReady) {
        if (passed != 0) {
          OnPopped(passed);
          passed = 0;
        }
        // Its producer is writing the item; a sleeper is woken when that
        // producer passes the lane turn.
        hold.woken = Wait(kFrontSleeper, [&] { return Probe(ln, li, cell) == Slot::kNotReady; }, hold.epoch,
                          hold.deadline);
        if (hold.woken != Woken::kRetry) {
          LetGo(ln, li);
          return Front::kNotReady;
        }
      }
      if (!head_.popped.compare_exchange_strong(ticket, ticket + 1, std::memory_order_seq_cst,
                                                std::memory_order_seq_cst)) {
        // TakeClaimed() removed it meanwhile and waits for the lane.
        LetGo(ln, li);
        continue;
      }
      ++passed;
      TakeSlot(take, ln, li, slot, cell);
      if (slot == Slot::kItem) return Front::kItem;
      ++ticket;
    }
  }

  // TryPop / Clear / the destructor.
  template <class Take>
  Front TakeClaimed(Take& take, size_t& passed) {
    uint64_t ticket = head_.popped.load(std::memory_order_relaxed);
    while (true) {
      do {
        if (!Pending(ticket)) return Front::kEmpty;
      } while (!head_.popped.compare_exchange_weak(ticket, ticket + 1, std::memory_order_seq_cst,
                                                   std::memory_order_relaxed));
      ++passed;
      Lane& ln = lanes_[ticket % kLanes];
      const uint64_t li = ticket / kLanes;
      // Before sleeping, report what was removed: a Pop() waiting for the
      // front, or a Push() waiting for a slot, can go on meanwhile.
      const auto flush = [&] {
        OnPopped(passed);
        passed = 0;
      };
      if (ln.head_turn.load(std::memory_order_seq_cst) != li) {
        flush();
        Wait(kFrontSleeper, [&] { return ln.head_turn.load(std::memory_order_seq_cst) != li; }, kIgnoreAbort,
             kNoDeadline);
      }
      Cell* cell = nullptr;
      Slot slot;
      while ((slot = Probe(ln, li, cell)) == Slot::kNotReady) {
        if (passed != 0) flush();
        Wait(kFrontSleeper, [&] { return Probe(ln, li, cell) == Slot::kNotReady; }, kIgnoreAbort, kNoDeadline);
      }
      TakeSlot(take, ln, li, slot, cell);
      // The caller reports `passed`; with nothing left to report, the lane's
      // next consumer still has to hear about head_turn.
      if (passed == 0) OnPopped(0);
      if (slot == Slot::kItem) return Front::kItem;
      ticket = head_.popped.load(std::memory_order_relaxed);
    }
  }

  // By the owner of lane ln for li, after the ticket is removed: hands the
  // item (if any) to take() and the lane to the next consumer.
  template <class Take>
  void TakeSlot(Take& take, Lane& ln, uint64_t li, Slot slot, Cell* cell) noexcept {
    if (slot == Slot::kItem) {
      T* p = cell->Item();
      take(*p);
      p->~T();
    } else {
      skipped_.fetch_sub(1, std::memory_order_relaxed);
    }
    ln.head_turn.store(li + 1, std::memory_order_release);
  }

  // TakeHeld() lets go of lane ln without having removed anything.
  void LetGo(Lane& ln, uint64_t li) {
    uint64_t busy = li | kBusy;
    ln.head_turn.compare_exchange_strong(busy, li, std::memory_order_release, std::memory_order_relaxed);
    OnPopped(0);
  }

  // Whether `ticket` has been handed to a producer. tail_ only grows, so a
  // ticket below a value once read is; head_.tail_seen keeps that value and
  // spares consumers a read of tail_, a line producers keep writing.
  bool Pending(uint64_t ticket) {
    if (ticket < head_.tail_seen.load(std::memory_order_relaxed)) return true;
    const uint64_t tail = tail_.load(std::memory_order_seq_cst);
    if (ticket >= tail) return false;
    head_.tail_seen.store(tail, std::memory_order_relaxed);
    return true;
  }

  // Under head_.mu (without kTicketedPop).
  template <class Take>
  Front PopFrontLocked(Take& take, size_t& passed) {
    while (true) {
      const uint64_t popped = head_.popped.load(std::memory_order_relaxed);
      if (!Pending(popped)) return Front::kEmpty;
      Cell* cell = nullptr;
      const Slot slot = Probe(lanes_[popped % kLanes], popped / kLanes, cell);
      if (slot == Slot::kNotReady) return Front::kNotReady;
      if (slot == Slot::kItem) {
        T* item = cell->Item();
        take(*item);
        item->~T();
        head_.popped.store(popped + 1, std::memory_order_seq_cst);  // see Wake()
        ++passed;
        return Front::kItem;
      }
      // Uncount before advancing, so Size() never subtracts it twice.
      skipped_.fetch_sub(1, std::memory_order_relaxed);
      head_.popped.store(popped + 1, std::memory_order_seq_cst);
      ++passed;
    }
  }

  // Called by the owner of the consumer side of lane ln (lane held, or
  // head_.mu). Moves head_block forward to the block holding li and reports
  // the cell; Slot::kSkipped with no cell when that push got no block.
  Slot Probe(Lane& ln, uint64_t li, Cell*& cell) {
    while (ln.head_seq != li / kBlockSlots) {
      Block* next = ln.head_block->next.load(std::memory_order_acquire);
      if (next == nullptr) {
        // Its producer links the block before passing the turn; a later
        // producer links it with li's cell marked skipped.
        if (ln.turn.load(std::memory_order_acquire) <= li) return Slot::kNotReady;
        next = ln.head_block->next.load(std::memory_order_acquire);
        if (next == nullptr) {
          cell = nullptr;
          return Slot::kSkipped;
        }
      }
      Recycle(ln, std::exchange(ln.head_block, next));
      ++ln.head_seq;
    }
    cell = &ln.head_block->cells[Index(li)];
    switch (cell->state.load(std::memory_order_acquire)) {
      case kFull:
        return Slot::kItem;
      case kSkipped:
        return Slot::kSkipped;
      default:
        return Slot::kNotReady;
    }
  }

  bool EmptyNow() const {
    return head_.popped.load(std::memory_order_seq_cst) == tail_.load(std::memory_order_seq_cst);
  }

  bool Full() const {
    const uint64_t popped = head_.popped.load(std::memory_order_seq_cst);
    const uint64_t pushed = tail_.load(std::memory_order_seq_cst);
    return pushed - popped >= capacity_.load(std::memory_order_seq_cst);
  }

  // After TakeFront found ticket t at the front but could not take it:
  // waits until the lane's consumer side is free (another consumer holds
  // it, or the lane's previous consumer is not done) or, without
  // kTicketedPop, until its push is finished; or until t is no longer the
  // front.
  Woken WaitFront(uint64_t t, uint64_t epoch, Clock::time_point deadline) {
    Lane& ln = lanes_[t % kLanes];
    const uint64_t li = t / kLanes;
    const auto front = [this, t] {
      return head_.popped.load(std::memory_order_acquire) == t && t < tail_.load(std::memory_order_acquire);
    };
    if constexpr (kTicketedPop) {
      return Wait(kFrontSleeper,
                  [&] { return ln.head_turn.load(std::memory_order_acquire) != li && front(); }, epoch, deadline);
    } else {
      return Wait(kFrontSleeper,
                  [&] { return ln.turn.load(std::memory_order_acquire) <= li && front(); }, epoch, deadline);
    }
  }

  // now + timeout, or kNoDeadline if that is past the clock's range. For
  // integer ticks the range check only divides, so it cannot overflow.
  template <class Rep, class Period>
  static Clock::time_point DeadlineAfter(std::chrono::duration<Rep, Period> timeout) {
    using std::chrono::duration;
    using ToClock = std::ratio_divide<Period, Clock::period>;
    const Clock::time_point now = Clock::now();
    if (!(timeout > timeout.zero())) return now;  // also NaN
    const Clock::duration left = kNoDeadline - now;
    if constexpr (!std::chrono::treat_as_floating_point_v<Rep> && ToClock::den == 1) {
      // Whole multiples of a clock tick (seconds, ms, ...): compare in them.
      using Wide = duration<std::common_type_t<Rep, Clock::rep>, Period>;
      const Wide wide(timeout);
      if (wide > std::chrono::floor<Wide>(left)) return kNoDeadline;
      return now + std::chrono::duration_cast<Clock::duration>(wide);
    } else if constexpr (!std::chrono::treat_as_floating_point_v<Rep> && ToClock::num == 1) {
      // Fractions of a tick (ps, ...): in ticks the timeout only gets smaller.
      const Clock::duration ticks = std::chrono::ceil<Clock::duration>(timeout);
      return ticks >= left ? kNoDeadline : now + ticks;
    } else {
      // Floating ticks or odd ratios; long double keeps ns exact to ~2^64.
      using Ld = duration<long double, Clock::period>;
      if (Ld(timeout) >= Ld(left)) return kNoDeadline;
      return now + std::chrono::ceil<Clock::duration>(Ld(timeout));
    }
  }

  // Waits while blocked(): spins, then counts itself in sleepers_ under
  // `kind` and sleeps on that kind's condition variable. kRetry once
  // blocked() is false, else why it stopped. A pop waiter sets
  // *signalled if it may have been signalled while asleep (see SleepPop()).
  template <class Blocked>
  Woken Wait(uint64_t kind, Blocked blocked, uint64_t epoch, Clock::time_point deadline,
             bool* signalled = nullptr) {
    std::condition_variable& cv = kind == kPopSleeper ? not_empty_ : kind == kPushSleeper ? not_full_ : progress_;
    const auto aborted = [&] { return epoch != kIgnoreAbort && abort_.load(std::memory_order_seq_cst) != epoch; };
    if (deadline != kNoDeadline && Clock::now() >= deadline) {
      // Nothing left to wait for (PopFor(0), or a retry after the deadline).
      if (!blocked()) return Woken::kRetry;
      return aborted() ? Woken::kAborted : Woken::kTimeout;
    }
    // Short waits (an item about to be written, a slot about to be freed)
    // end here, without a futex sleep on this side or a wake on the other.
    // A pop waiter is counted in pop_spinners_ until it is counted in
    // sleepers_ or stops waiting (see Wake()).
    YieldBudget* budget = kind == kPopSleeper ? &pop_yields_ : kind == kPushSleeper ? &push_yields_ : nullptr;
    // Looked up after the spins, by a pop waiter only.
    detail::SlowFlag<kSlowFlagBits> slow;
    // Yields from the budget, and in all; set after the spins.
    int budgeted = 0;
    int yields = 0;
    struct Spinning {
      explicit Spinning(std::atomic<uint64_t>* s) : spinners(s) {
        if (spinners != nullptr) spinners->fetch_add(1, std::memory_order_seq_cst);
      }
      ~Spinning() { Stop(); }
      void Stop() {
        if (spinners != nullptr) spinners->fetch_sub(1, std::memory_order_seq_cst);
        spinners = nullptr;
      }
      std::atomic<uint64_t>* spinners;
    } spinning{kind == kPopSleeper ? &pop_spinners_ : nullptr};
    if (kind == kPopSleeper) IBMW_SELFDEV_BBQ_RACE_POINT(kCounted);  // counted in pop_spinners_, before any spin
    for (int n = 0; n < kWaitSpins + yields; ++n) {
      if (n < kWaitSpins) {
        detail::CpuRelax();
      } else {
        const Clock::time_point before = Clock::now();
        std::this_thread::yield();
        slow.Set(Clock::now() - before > kYieldStall);
      }
      if (!blocked()) {
        if (budget != nullptr && n < kWaitSpins + budgeted) budget->Hit();
        return Woken::kRetry;
      }
      if (aborted()) return Woken::kAborted;
      if (n == kWaitSpins - 1) {
        if (kind == kPopSleeper) slow.Bind(slow_);
        // While skips are off every push wakes a sleeper, as with tbb: then
        // yielding first only costs CPU (twice tbb's in the sparse cases).
        if (kind == kPopSleeper && skip_gate_.Off()) budget = nullptr;
        budgeted = budget != nullptr ? budget->Get() : kind == kFrontSleeper ? kFrontYields : 0;
        yields = slow.Get() ? std::max(budgeted, kMinYields) : budgeted;
      }
      // A yield can take a whole time slice.
      if (n >= kWaitSpins && deadline != kNoDeadline && Clock::now() >= deadline) return Woken::kTimeout;
    }
    if (budget != nullptr) budget->Miss();
    IBMW_SELFDEV_BBQ_RACE_POINT(kSpun);  // still counted in pop_spinners_
    // Counted, and for a front sleeper the HeavyBarrier() (an IPI to every
    // running thread, microseconds) done, before taking wait_mu_, so other
    // sleepers and wakers do not wait for it; see Wake() for why blocked()
    // need only be re-checked under the lock.
    sleepers_.fetch_add(kind, std::memory_order_seq_cst);
    spinning.Stop();
    struct Uncount {  // also if taking the lock throws
      std::atomic<uint64_t>& sleepers;
      uint64_t kind;
      ~Uncount() { sleepers.fetch_sub(kind, std::memory_order_relaxed); }
    } uncount{sleepers_, kind};
    if (kind == kFrontSleeper && !detail::HeavyBarrier()) {  // see Wake()
      return PollFront(cv, blocked, aborted, deadline);
    }
    std::unique_lock<std::mutex> lock(wait_mu_);
    Woken woken = Woken::kRetry;
    bool timed = true;
    while (blocked()) {
      if (aborted()) {
        woken = Woken::kAborted;
        break;
      }
      // Only a signal since this sleep began tells how soon it ran (see
      // kWakeStall); not one before a backstop timeout or spurious wakeup.
      const Clock::duration asleep = kind == kPopSleeper ? Clock::now().time_since_epoch() : Clock::duration::zero();
      const bool slept =
          kind == kPopSleeper ? SleepPop(lock, deadline, asleep, timed, signalled) : SleepUntil(cv, lock, deadline);
      if (!slept) {
        if (blocked()) woken = aborted() ? Woken::kAborted : Woken::kTimeout;
        break;
      }
      if (kind == kPopSleeper && !blocked()) {
        const Clock::duration at(pop_signalled_.load(std::memory_order_relaxed));
        if (at >= asleep) slow.Set(Clock::now().time_since_epoch() - at > kWakeStall);
      }
    }
    return woken;
  }

  // SleepUntil() on not_empty_, for at most kBackstop while a pop waiter
  // spins and wakes may be skipped, counted in timed_pop_sleepers_
  // meanwhile; tells skip_gate_ if items are still queued after that. Once
  // such a sleep ends with none queued, `timed` is false: the caller's
  // further sleeps are untimed, so that sleepers next to a consumer that
  // stays counted as spinning (it does not get the CPU) do not wake every
  // kBackstop while the queue is empty. A sleep begun `asleep` (under the
  // lock), timed or not, sets *signalled if a pop sleeper was signalled
  // since: the item this call then takes may be one whose wake was
  // skipped (see Wake()), while the signal was for another. A timed sleep
  // may report a timeout although it was signalled (wait_until() judges by
  // the clock after the wait). Not after a timeout with no signal: passing
  // wakes on then made bursts late next to starved consumers (BQ-47).
  bool SleepPop(std::unique_lock<std::mutex>& lock, Clock::time_point deadline, Clock::duration asleep, bool& timed,
                bool* signalled) {
    const auto note_signal = [&] {
      if (signalled != nullptr && Clock::duration(pop_signalled_.load(std::memory_order_relaxed)) >= asleep) {
        *signalled = true;
      }
    };
    if (!timed || pop_spinners_.load(std::memory_order_seq_cst) == 0 || skip_gate_.Off()) {
      IBMW_SELFDEV_BBQ_RACE_POINT(kAsleep);  // under wait_mu_, as kAsleepTimed and kTimedWoken
      const bool woken = SleepUntil(not_empty_, lock, deadline);
      note_signal();
      return woken;
    }
    struct Timed {
      std::atomic<uint64_t>& sleepers;
      explicit Timed(std::atomic<uint64_t>& s) : sleepers(s) { sleepers.fetch_add(1, std::memory_order_seq_cst); }
      ~Timed() { sleepers.fetch_sub(1, std::memory_order_seq_cst); }
    } counted{timed_pop_sleepers_};
    const Clock::time_point until = std::min(deadline, Clock::time_point(asleep) + kBackstop);
    IBMW_SELFDEV_BBQ_RACE_POINT(kAsleepTimed);
    const bool woken = SleepUntil(not_empty_, lock, until);
    IBMW_SELFDEV_BBQ_RACE_POINT(kTimedWoken);
    note_signal();
    if (woken) return true;
    if (until == deadline) return false;
    if (EmptyNow()) {
      timed = false;
    } else {
      skip_gate_.Missed(head_.popped.load(std::memory_order_relaxed));
    }
    return true;
  }

  // The rest of Wait() for a front sleeper whose HeavyBarrier() failed: a
  // waker's change may then go unseen when it is signalled, so re-check
  // every kUnpairedPoll. Kept out of line: inlining it into Wait() (where
  // it never runs) made 1P8C in the queue benchmark about 40% slower (GCC 11).
  template <class Blocked, class Aborted>
  [[gnu::noinline]] Woken PollFront(std::condition_variable& cv, Blocked& blocked, Aborted& aborted,
                                    Clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(wait_mu_);
    while (blocked()) {
      if (aborted()) return Woken::kAborted;
      const Clock::time_point until = std::min(deadline, Clock::now() + kUnpairedPoll);
      if (!SleepUntil(cv, lock, until) && until == deadline) {
        if (blocked()) return aborted() ? Woken::kAborted : Woken::kTimeout;
        break;
      }
    }
    return Woken::kRetry;
  }

  // False once the deadline has passed.
  static bool SleepUntil(std::condition_variable& cv, std::unique_lock<std::mutex>& lock,
                         Clock::time_point deadline) {
    if (deadline == kNoDeadline) {
      cv.wait(lock);
      return true;
    }
    if constexpr (detail::kThreadSanitizer) {
      // A steady_clock wait is pthread_cond_clockwait, which GCC 11's
      // libtsan does not intercept (it then reports a false double lock).
      // Wait on the system clock (pthread_cond_timedwait) in steps of at
      // most 1 s and judge the deadline on the steady clock, so a system
      // clock jump delays the end by at most one step.
      const Clock::duration left = std::min<Clock::duration>(deadline - Clock::now(), std::chrono::seconds(1));
      cv.wait_until(lock, std::chrono::system_clock::now() + left);
      return Clock::now() < deadline;
    } else {
      return cv.wait_until(lock, deadline) == std::cv_status::no_timeout;
    }
  }

  // Sleep handshake. A sleeper counts itself in sleepers_ and then re-reads
  // what it waits on (under wait_mu_); a waker changes that and
  // then reads sleepers_, so one of them sees the other: the sleeper does
  // not sleep, or the waker signals it. Pop and push sleepers wait on tail_
  // and head_.popped, which only change by seq_cst writes, as sleepers_ is
  // accessed. Front sleepers also wait on a lane's turn or head_turn, or a
  // cell, written with release stores on every push and pop: there the
  // waker puts LightBarrier() between its stores and the read of sleepers_,
  // and the sleeper HeavyBarrier() after counting itself (if that fails,
  // the sleeper does not rely on the signal and re-checks periodically).
  //
  // Wakes front sleepers and `n` sleepers of `kind`. A counted sleeper
  // checks blocked() under wait_mu_: before the waker takes wait_mu_ (the
  // sleeper is then inside wait() when signalled) or after the waker has
  // released it (it then sees the waker's change, made before).
  //
  // One pop sleeper (n == 1, no front sleeper) is not woken while a pop
  // waiter spins and a pop sleeper sleeps for at most kBackstop
  // (timed_pop_sleepers_), unless skip_gate_ says no (an item of an earlier
  // skip has been queued kSkipGrace, or a skip lately held an item back).
  // A spinner leaves pop_spinners_ before it looks for an item (TryPopImpl,
  // or blocked() under wait_mu_ once counted in sleepers_), a timed sleeper
  // timed_pop_sleepers_ before it re-checks blocked() under wait_mu_; all
  // seq_cst, like the pusher's ticket and its reads of the two counts, so
  // if the pusher saw them counted they find an item: the spinner if it
  // runs, the timed sleeper within kBackstop. That item may be another
  // one: of an earlier skip, or one the timed sleeper was signalled for. So
  // a consumer that took an item after waiting wakes a sleeper if items
  // are left and skip_gate_ shows a skip of an item it may not have taken
  // (PopUntil()); a signalled one also if its own item may be skipped, as
  // the signal was then for another item. The pusher re-reads the counts
  // after recording the skip (seq_cst, as the taker uncounts itself
  // before it reads skip_gate_): a waiter it saw counted that took an item
  // without seeing the skip has uncounted itself by then. Without that, a
  // pusher preempted between the two left its item to the timed sleeper's
  // kBackstop.
  void Wake(uint64_t kind, size_t n) {
    const uint64_t w = sleepers_.load(std::memory_order_seq_cst);
    if ((w & (kind * kSleeperMask | kFrontSleeper * kSleeperMask)) == 0) return;
    if (kind == kPopSleeper && n == 1 && (w & kFrontSleeper * kSleeperMask) == 0 && SkipPopWake()) return;
    WakeSlow(w, kind, n);
  }

  // Whether the wake of one pop sleeper may be skipped (see Wake()).
  bool SkipPopWake() {
    if (!PopWaitersCover()) return false;
    IBMW_SELFDEV_BBQ_RACE_POINT(kCovered);
    if (!skip_gate_.Skip(head_.popped.load(std::memory_order_relaxed), tail_.load(std::memory_order_relaxed))) {
      return false;
    }
    IBMW_SELFDEV_BBQ_RACE_POINT(kSkipped);
    return PopWaitersCover();
  }

  // Whether a pop waiter spins and a pop sleeper sleeps for at most
  // kBackstop.
  bool PopWaitersCover() const {
    return pop_spinners_.load(std::memory_order_seq_cst) != 0 &&
           timed_pop_sleepers_.load(std::memory_order_seq_cst) != 0;
  }

  [[gnu::noinline]] void WakeSlow(uint64_t w, uint64_t kind, size_t n) {
    const bool signal = n != 0 && (w & kind * kSleeperMask) != 0;
    {
      std::lock_guard<std::mutex> lock(wait_mu_);
      // Under the lock, so that a pop sleeper this signals, which took it
      // before its sleep began and re-takes it after, sees a later time.
      if (signal && kind == kPopSleeper) {
        pop_signalled_.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
      }
    }
    if ((w & kFrontSleeper * kSleeperMask) != 0) progress_.notify_all();
    if (signal) {
      std::condition_variable& cv = kind == kPopSleeper ? not_empty_ : not_full_;
      IBMW_SELFDEV_BBQ_RACE_POINT(kSignal);
      if (n > 1) {
        cv.notify_all();
      } else {
        cv.notify_one();
      }
    }
  }

  // After consumers removed `passed` tickets, or let go of a lane.
  void OnPopped(size_t passed) {
    detail::LightBarrier();
    Wake(kPushSleeper, passed);
  }

  static Block* TakeSpare(Lane& ln) {
    Block* b = ln.spare.exchange(nullptr, std::memory_order_acquire);
    if (b == nullptr) return new Block;
    b->next.store(nullptr, std::memory_order_relaxed);
    for (Cell& c : b->cells) c.state.store(kEmpty, std::memory_order_relaxed);
    return b;
  }

  static void Recycle(Lane& ln, Block* b) noexcept { delete ln.spare.exchange(b, std::memory_order_acq_rel); }

  struct alignas(64) Head {
    // Only used without kTicketedPop.
    detail::AdaptiveMutex mu;
    // Next ticket for consumers. Read anywhere (Size(), the sleep
    // handshake, a producer that finds the queue full).
    std::atomic<uint64_t> popped{0};
    std::atomic<uint64_t> tail_seen{0};
  };

  Head head_;
  // Next ticket to hand out.
  alignas(64) std::atomic<uint64_t> tail_{0};
  // Read by producers on every push; written rarely.
  alignas(64) std::atomic<size_t> capacity_;
  // Even: running; odd: aborted. Abort() and Reset() each add one, so a
  // blocked call sees any Abort() since it started.
  std::atomic<uint64_t> abort_{0};
  // Tickets in [popped, tail) whose push threw.
  std::atomic<uint64_t> skipped_{0};
  // Bounded mode only: a value head_.popped had, refreshed when the queue
  // looks full by it. Written by producers, so kept off the line above.
  alignas(64) std::atomic<uint64_t> popped_cache_{0};
  Lane lanes_[kLanes];
  // Read after every push / pop; written by threads going to sleep.
  alignas(64) std::atomic<uint64_t> sleepers_{0};
  // Pop waiters spinning; read by Wake() when there are pop sleepers.
  std::atomic<uint64_t> pop_spinners_{0};
  // Pop sleepers in SleepPop()'s timed sleep; read by Wake() likewise.
  std::atomic<uint64_t> timed_pop_sleepers_{0};
  // Written by Wake() while pop waiters spin and sleep, and by pop sleepers.
  SkipGate skip_gate_;
  // Clock::rep of the last signal to pop sleepers; read by woken ones.
  std::atomic<Clock::rep> pop_signalled_{0};
  // Sleeping only; never held together with head_.mu.
  alignas(64) std::mutex wait_mu_;
  YieldBudget pop_yields_;
  YieldBudget push_yields_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  // A lane turn, a lane head_turn or head_.popped moved.
  std::condition_variable progress_;
  // Pop waiters' threads that get little CPU (see kMinYields): one flag per
  // hash of the thread id, holding the mark of the thread that set it (0:
  // none), so that a thread clears only its own (see detail::SlowFlag).
  // Two such threads that share a flag take it in turns. A mark outlives
  // its thread: one that reuses the id inherits it, for one wait. Per
  // queue: a thread finds out anew at each. Not a thread_local: where TLS
  // is emulated (OHOS: a call to __emutls_get_address per access), one
  // made queue/4P4C/unbounded/std::function in queue_bench about 10%
  // slower on the board.
  alignas(64) std::atomic<uint8_t> slow_[1 << kSlowFlagBits] = {};
};

}  // namespace IBMW_SELFDEV_BARRIER_NS
}  // namespace ibmw::selfdev::concurrent
