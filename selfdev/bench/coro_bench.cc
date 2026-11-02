// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// selfdev/coro vs libunifex for the primitives IBMW uses through ibmw::co
// (Task, SyncWait, AsyncScope spawn/cleanup, On(scheduler, x) | Then(f)).
// Both sides schedule onto the same thread pool through intrusive queue
// items, so only the coroutine / sender machinery differs.

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <unifex/async_scope.hpp>
#include <unifex/on.hpp>
#include <unifex/receiver_concepts.hpp>
#include <unifex/sync_wait.hpp>
#include <unifex/task.hpp>
#include <unifex/then.hpp>

#include "bench_harness.h"
#include "ibmw_selfdev/coro/coro.h"

// ---------------------------------------------------------------------------
// Allocation counting (global operator new/delete replacement).

namespace {
std::atomic<uint64_t> g_alloc_count{0};
std::atomic<uint64_t> g_alloc_bytes{0};
}  // namespace

void* operator new(std::size_t n) {
  g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  g_alloc_bytes.fetch_add(n, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

namespace bench = ibmw::selfdev::bench;
namespace sd = ibmw::selfdev::coro;

// ---------------------------------------------------------------------------
// Shared pool: mutex + condvar + deque of intrusive items.

struct PoolItem {
  void (*run)(PoolItem*) noexcept;
};

class Pool {
 public:
  explicit Pool(size_t threads) {
    for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { Run(); });
  }
  ~Pool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

  void Post(PoolItem* item) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      items_.push_back(item);
    }
    cv_.notify_one();
  }

 private:
  void Run() {
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
      if (items_.empty()) {
        if (stop_) return;
        cv_.wait(lock);
        continue;
      }
      PoolItem* item = items_.front();
      items_.pop_front();
      lock.unlock();
      item->run(item);
      lock.lock();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<PoolItem*> items_;
  bool stop_ = false;
  std::vector<std::thread> workers_;
};

// selfdev scheduler: schedule() returns an awaiter that posts itself.
class SdScheduler {
 public:
  explicit SdScheduler(Pool* pool) noexcept : pool_(pool) {}

  struct Awaiter : PoolItem {
    Pool* pool;
    std::coroutine_handle<> h;
    explicit Awaiter(Pool* p) noexcept : PoolItem{&Resume}, pool(p) {}
    static void Resume(PoolItem* self) noexcept { static_cast<Awaiter*>(self)->h.resume(); }
    bool await_ready() const noexcept { return false; }
    // Awaiter rule 2 (coro/README.md): a worker may resume h, and destroy
    // the frame holding *this, while Post() is still running.
    IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> handle) {
      Pool* const p = pool;
      h = handle;
      p->Post(this);
    }
    void await_resume() const noexcept {}
  };

  Awaiter schedule() const noexcept { return Awaiter(pool_); }
  // Required by the scheduler concepts; clang sees no direct use.
  [[maybe_unused]] friend bool operator==(const SdScheduler& a, const SdScheduler& b) noexcept {
    return a.pool_ == b.pool_;
  }

 private:
  Pool* pool_;
};

// unifex scheduler: schedule() returns a sender whose operation state posts
// itself and calls set_value on the pool thread.
class UxScheduler {
 public:
  explicit UxScheduler(Pool* pool) noexcept : pool_(pool) {}

  template <class Receiver>
  struct Op : PoolItem {
    Pool* pool;
    Receiver receiver;
    template <class R>
    Op(Pool* p, R&& r) : PoolItem{&Run}, pool(p), receiver(static_cast<R&&>(r)) {}
    static void Run(PoolItem* self) noexcept {
      auto* op = static_cast<Op*>(self);
      try {
        unifex::set_value(std::move(op->receiver));
      } catch (...) {
        unifex::set_error(std::move(op->receiver), std::current_exception());
      }
    }
    void start() noexcept { pool->Post(this); }
  };

  struct Sender {
    template <template <typename...> class Variant, template <typename...> class Tuple>
    using value_types = Variant<Tuple<>>;
    template <template <typename...> class Variant>
    using error_types = Variant<std::exception_ptr>;
    static constexpr bool sends_done = false;

    Pool* pool;
    template <class Receiver>
    Op<unifex::remove_cvref_t<Receiver>> connect(Receiver&& r) const {
      return Op<unifex::remove_cvref_t<Receiver>>(pool, static_cast<Receiver&&>(r));
    }
  };

  Sender schedule() const noexcept { return Sender{pool_}; }
  // Required by the scheduler concepts; clang sees no direct use.
  [[maybe_unused]] friend bool operator==(const UxScheduler& a, const UxScheduler& b) noexcept {
    return a.pool_ == b.pool_;
  }
  // Required by the scheduler concepts; clang sees no direct use.
  [[maybe_unused]] friend bool operator!=(const UxScheduler& a, const UxScheduler& b) noexcept {
    return a.pool_ != b.pool_;
  }

 private:
  Pool* pool_;
};

// ---------------------------------------------------------------------------
// Trivial coroutines.

sd::Task<int> SdLeaf(int v) { co_return v + 1; }
unifex::task<int> UxLeaf(int v) { co_return v + 1; }

sd::Task<void> SdVoid() { co_return; }
unifex::task<void> UxVoid() { co_return; }

sd::Task<int64_t> SdChain(int64_t n) {
  int64_t sum = 0;
  for (int64_t i = 0; i < n; ++i) sum += co_await SdLeaf(static_cast<int>(i & 0xff));
  co_return sum;
}
unifex::task<int64_t> UxChain(int64_t n) {
  int64_t sum = 0;
  for (int64_t i = 0; i < n; ++i) sum += co_await UxLeaf(static_cast<int>(i & 0xff));
  co_return sum;
}

std::atomic<uint64_t> g_then_sink{0};

// ---------------------------------------------------------------------------
// Scenarios. Each returns the number of operations.

uint64_t SdChainRun(uint64_t n) {
  auto r = sd::SyncWait(SdChain(static_cast<int64_t>(n)));
  bench::DoNotOptimize(*r);
  return n;
}
uint64_t UxChainRun(uint64_t n) {
  auto r = unifex::sync_wait(UxChain(static_cast<int64_t>(n)));
  bench::DoNotOptimize(*r);
  return n;
}

uint64_t SdSyncWaitRun(uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) bench::DoNotOptimize(*sd::SyncWait(SdLeaf(static_cast<int>(i))));
  return n;
}
uint64_t UxSyncWaitRun(uint64_t n) {
  for (uint64_t i = 0; i < n; ++i) bench::DoNotOptimize(*unifex::sync_wait(UxLeaf(static_cast<int>(i))));
  return n;
}

uint64_t SdSpawnRun(uint64_t n) {
  sd::AsyncScope scope;
  for (uint64_t i = 0; i < n; ++i) scope.spawn(SdVoid());
  sd::SyncWait(scope.cleanup());
  return n;
}
uint64_t UxSpawnRun(uint64_t n) {
  unifex::async_scope scope;
  for (uint64_t i = 0; i < n; ++i) scope.spawn(UxVoid());
  unifex::sync_wait(scope.cleanup());
  return n;
}

// Round trip: caller blocks until On(pool, leaf) | Then(f) completes.
uint64_t SdOnThenSyncRun(Pool& pool, uint64_t n) {
  SdScheduler s(&pool);
  for (uint64_t i = 0; i < n; ++i) {
    auto r = sd::SyncWait(sd::On(s, SdLeaf(static_cast<int>(i))) | sd::Then([](int v) { return v * 2; }));
    bench::DoNotOptimize(*r);
  }
  return n;
}
uint64_t UxOnThenSyncRun(Pool& pool, uint64_t n) {
  UxScheduler s(&pool);
  for (uint64_t i = 0; i < n; ++i) {
    auto r = unifex::sync_wait(unifex::on(s, UxLeaf(static_cast<int>(i))) | unifex::then([](int v) { return v * 2; }));
    bench::DoNotOptimize(*r);
  }
  return n;
}

// Fire-and-forget throughput, the shape of generated RPC code:
// StartDetached(On(exe, handler()) | Then(f)); here on a local scope.
uint64_t SdOnThenSpawnRun(Pool& pool, uint64_t n) {
  SdScheduler s(&pool);
  sd::AsyncScope scope;
  for (uint64_t i = 0; i < n; ++i) {
    scope.spawn(sd::On(s, SdLeaf(static_cast<int>(i))) |
                sd::Then([](int v) { g_then_sink.fetch_add(static_cast<uint64_t>(v), std::memory_order_relaxed); }));
  }
  sd::SyncWait(scope.cleanup());
  return n;
}
uint64_t UxOnThenSpawnRun(Pool& pool, uint64_t n) {
  UxScheduler s(&pool);
  unifex::async_scope scope;
  for (uint64_t i = 0; i < n; ++i) {
    scope.spawn(unifex::on(s, UxLeaf(static_cast<int>(i))) | unifex::then([](int v) {
                  g_then_sink.fetch_add(static_cast<uint64_t>(v), std::memory_order_relaxed);
                }));
  }
  unifex::sync_wait(scope.cleanup());
  return n;
}

// Allocations per operation, measured on a single untimed pass.
template <class Fn>
void ReportAllocs(const char* name, uint64_t n, Fn fn) {
  const uint64_t c0 = g_alloc_count.load();
  const uint64_t b0 = g_alloc_bytes.load();
  fn(n);
  const uint64_t c1 = g_alloc_count.load();
  const uint64_t b1 = g_alloc_bytes.load();
  std::printf("# allocs %-52s %8.2f allocs/op %10.1f bytes/op\n", name,
              static_cast<double>(c1 - c0) / static_cast<double>(n),
              static_cast<double>(b1 - b0) / static_cast<double>(n));
}

}  // namespace

int main(int argc, char** argv) {
  bench::Runner r(bench::ParseOptions(argc, argv));

  // (e) Frame sizes: bytes requested by operator new for one trivial task.
  {
    const uint64_t b0 = g_alloc_bytes.load();
    { auto t = SdLeaf(1); }
    const uint64_t b1 = g_alloc_bytes.load();
    { auto t = UxLeaf(1); }
    const uint64_t b2 = g_alloc_bytes.load();
    std::printf("# frame bytes Task<int> trivial: selfdev=%llu unifex=%llu\n",
                static_cast<unsigned long long>(b1 - b0), static_cast<unsigned long long>(b2 - b1));
    std::printf("# sizeof promise: selfdev TaskPromise<int>=%zu unifex _promise<int>=%zu\n",
                sizeof(sd::Task<int>::promise_type), sizeof(unifex::task<int>::promise_type));
  }
  {
    Pool p1(1);
    ReportAllocs("chain co_await [selfdev]", 10000, SdChainRun);
    ReportAllocs("chain co_await [unifex]", 10000, UxChainRun);
    ReportAllocs("SyncWait(trivial) [selfdev]", 10000, SdSyncWaitRun);
    ReportAllocs("SyncWait(trivial) [unifex]", 10000, UxSyncWaitRun);
    ReportAllocs("spawn+cleanup [selfdev]", 10000, SdSpawnRun);
    ReportAllocs("spawn+cleanup [unifex]", 10000, UxSpawnRun);
    ReportAllocs("On|Then SyncWait [selfdev]", 10000, [&](uint64_t n) { return SdOnThenSyncRun(p1, n); });
    ReportAllocs("On|Then SyncWait [unifex]", 10000, [&](uint64_t n) { return UxOnThenSyncRun(p1, n); });
    ReportAllocs("On|Then spawn [selfdev]", 10000, [&](uint64_t n) { return SdOnThenSpawnRun(p1, n); });
    ReportAllocs("On|Then spawn [unifex]", 10000, [&](uint64_t n) { return UxOnThenSpawnRun(p1, n); });
  }

  r.Compare("coro/a chain co_await trivial Task x1M", [&] { return SdChainRun(r.N(1'000'000)); },
            [&] { return UxChainRun(r.N(1'000'000)); }, "unifex");
  r.Compare("coro/b SyncWait(trivial Task)", [&] { return SdSyncWaitRun(r.N(1'000'000)); },
            [&] { return UxSyncWaitRun(r.N(1'000'000)); }, "unifex");
  r.Compare("coro/c AsyncScope spawn trivial x100k + cleanup", [&] { return SdSpawnRun(r.N(100'000)); },
            [&] { return UxSpawnRun(r.N(100'000)); }, "unifex");
  for (size_t threads : {size_t{1}, size_t{4}}) {
    Pool pool(threads);
    const std::string th = std::to_string(threads) + "thr";
    r.Compare("coro/d1 SyncWait(On(pool,t)|Then(f)) round trip/" + th,
              [&] { return SdOnThenSyncRun(pool, r.N(100'000)); }, [&] { return UxOnThenSyncRun(pool, r.N(100'000)); },
              "unifex");
    r.Compare("coro/d2 spawn(On(pool,t)|Then(f)) x100k + cleanup/" + th,
              [&] { return SdOnThenSpawnRun(pool, r.N(100'000)); },
              [&] { return UxOnThenSpawnRun(pool, r.N(100'000)); }, "unifex");
  }
  return 0;
}
