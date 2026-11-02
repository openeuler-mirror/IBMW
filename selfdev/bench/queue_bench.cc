// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// BoundedBlockingQueue vs tbb::concurrent_bounded_queue, with the access
// pattern of IBMW's ParallelExecutor: producers Emplace tasks, each consumer
// drains with TryPop and then blocks in Pop.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "bench_harness.h"
#include "ibmw_selfdev/concurrent/bounded_blocking_queue.h"
#include "oneapi/tbb/concurrent_queue.h"

namespace {

namespace bench = ibmw::selfdev::bench;

// Same layout as ibmw::util::Function / ibmw::executor::Task: one ops pointer
// plus a 48-byte local buffer, relocated (not copied) on move.
class SboTask {
 public:
  static constexpr size_t kBufSize = 48;

  SboTask() noexcept = default;

  template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, SboTask>>>
  explicit SboTask(F&& f) {
    using D = std::decay_t<F>;
    static_assert(sizeof(D) <= kBufSize && std::is_nothrow_move_constructible_v<D>);
    static constexpr Ops kOps{
        [](void* buf) { (*static_cast<D*>(buf))(); },
        [](void* from, void* to) noexcept {
          new (to) D(std::move(*static_cast<D*>(from)));
          static_cast<D*>(from)->~D();
        },
        [](void* buf) noexcept { static_cast<D*>(buf)->~D(); }};
    new (buf_) D(std::forward<F>(f));
    ops_ = &kOps;
  }

  SboTask(SboTask&& o) noexcept : ops_(std::exchange(o.ops_, nullptr)) {
    if (ops_ != nullptr) ops_->relocate(o.buf_, buf_);
  }

  SboTask& operator=(SboTask&& o) noexcept {
    if (this != &o) {
      Reset();
      ops_ = std::exchange(o.ops_, nullptr);
      if (ops_ != nullptr) ops_->relocate(o.buf_, buf_);
    }
    return *this;
  }

  SboTask(const SboTask&) = delete;
  SboTask& operator=(const SboTask&) = delete;

  ~SboTask() { Reset(); }

  void operator()() { ops_->invoke(buf_); }
  explicit operator bool() const noexcept { return ops_ != nullptr; }

 private:
  struct Ops {
    void (*invoke)(void*);
    void (*relocate)(void*, void*) noexcept;
    void (*destroy)(void*) noexcept;
  };

  void Reset() noexcept {
    if (ops_ != nullptr) std::exchange(ops_, nullptr)->destroy(buf_);
  }

  const Ops* ops_ = nullptr;
  alignas(std::max_align_t) unsigned char buf_[kBufSize];
};

thread_local uint64_t tl_sink = 0;

// Iterations of a dependent multiply chain (~1 ns each on the bench machine)
// run by every task and by producers between pushes; 0 = pure queue cost.
// Written before the threads start.
int g_work = 0;

inline void Work() {
  uint64_t x = tl_sink | 1;
  for (int i = 0; i < g_work; ++i) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
  tl_sink = x;
}

// 40 bytes of captures: fits SboTask's buffer, exceeds std::function's
// 16-byte local storage (libstdc++), as real executor lambdas usually do.
template <class P>
P MakeTask(uint64_t i) {
  uint64_t a = i, b = i * 3, c = i ^ 0x5a5a, d = i + 7, e = i >> 1;
  return P([a, b, c, d, e] {
    tl_sink += a + b + c + d + e;
    Work();
  });
}

struct SelfdevQ {
  template <class P>
  struct Q {
    explicit Q(size_t cap)
        : q(cap == 0 ? ibmw::selfdev::concurrent::BoundedBlockingQueue<P>::kUnbounded : cap) {}
    void Push(P&& p) { q.Emplace(std::move(p)); }
    bool TryPop(P& p) { return q.TryPop(p); }
    void Pop(P& p) { q.Pop(p); }
    ibmw::selfdev::concurrent::BoundedBlockingQueue<P> q;
  };
};

struct TbbQ {
  template <class P>
  struct Q {
    explicit Q(size_t cap) {
      if (cap != 0) q.set_capacity(static_cast<std::ptrdiff_t>(cap));
    }
    void Push(P&& p) { q.emplace(std::move(p)); }
    bool TryPop(P& p) { return q.try_pop(p); }
    void Pop(P& p) { q.pop(p); }
    tbb::concurrent_bounded_queue<P> q;
  };
};

// `total` tasks spread over `producers`; each consumer runs the executor loop
// until it pops an empty task (one per consumer, pushed after all producers).
template <class Impl, class P>
bench::Sample RunMpmc(int producers, int consumers, size_t capacity, uint64_t total, int work = 0) {
  g_work = work;
  typename Impl::template Q<P> q(capacity);
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int c = 0; c < consumers; ++c) {
    threads.emplace_back([&] {
      ready.fetch_add(1);
      while (!go.load(std::memory_order_acquire)) {
      }
      P task;
      while (true) {
        while (q.TryPop(task)) {
          if (!task) return;
          task();
        }
        q.Pop(task);
        if (!task) return;
        task();
      }
    });
  }
  std::vector<std::thread> prods;
  for (int p = 0; p < producers; ++p) {
    prods.emplace_back([&, p] {
      ready.fetch_add(1);
      while (!go.load(std::memory_order_acquire)) {
      }
      const uint64_t n = total / static_cast<uint64_t>(producers);
      for (uint64_t i = 0; i < n; ++i) {
        Work();
        q.Push(MakeTask<P>(i + static_cast<uint64_t>(p)));
      }
    });
  }
  while (ready.load() != producers + consumers) {
  }
  const bench::Stopwatch sw;
  go.store(true, std::memory_order_release);
  for (auto& t : prods) t.join();
  for (int c = 0; c < consumers; ++c) q.Push(P());
  for (auto& t : threads) t.join();
  const double ns = sw.ElapsedNs();
  g_work = 0;
  return {(total / static_cast<uint64_t>(producers)) * static_cast<uint64_t>(producers), ns};
}

// Single thread: push one task, pop it, run it.
template <class Impl, class P>
uint64_t RunUncontended(uint64_t n) {
  typename Impl::template Q<P> q(0);
  P task;
  for (uint64_t i = 0; i < n; ++i) {
    q.Push(MakeTask<P>(i));
    q.TryPop(task);
    task();
  }
  bench::DoNotOptimize(tl_sink);
  return n;
}

template <class P>
void RunAll(bench::Runner& r, const char* payload) {
  const std::string pl = payload;
  r.Compare("queue/uncontended push+pop/" + pl, [&] { return RunUncontended<SelfdevQ, P>(r.N(2'000'000)); },
            [&] { return RunUncontended<TbbQ, P>(r.N(2'000'000)); }, "tbb");
  struct Shape {
    int p, c;
  };
  for (Shape s : {Shape{1, 1}, Shape{4, 4}, Shape{1, 8}}) {
    for (size_t cap : {size_t{0}, size_t{1024}}) {
      const std::string name = "queue/" + std::to_string(s.p) + "P" + std::to_string(s.c) + "C/" +
                               (cap == 0 ? "unbounded" : "cap1024") + "/" + pl;
      r.Compare(name, [&] { return RunMpmc<SelfdevQ, P>(s.p, s.c, cap, r.N(1'000'000)); },
                [&] { return RunMpmc<TbbQ, P>(s.p, s.c, cap, r.N(1'000'000)); }, "tbb");
    }
  }
  // Executor tasks do real work; with some per-task (and per-submit) work
  // the threads meet on the queue far less often than in the loops above.
  for (int work : {200, 1000}) {
    const std::string name = "queue/4P4C/unbounded/work" + std::to_string(work) + "/" + pl;
    r.Compare(name, [&] { return RunMpmc<SelfdevQ, P>(4, 4, 0, r.N(400'000), work); },
              [&] { return RunMpmc<TbbQ, P>(4, 4, 0, r.N(400'000), work); }, "tbb");
  }
}

}  // namespace

int main(int argc, char** argv) {
  bench::Runner r(bench::ParseOptions(argc, argv));
  std::printf("# sizeof(SboTask)=%zu sizeof(std::function<void()>)=%zu\n", sizeof(SboTask),
              sizeof(std::function<void()>));
  RunAll<SboTask>(r, "sbo48");
  RunAll<std::function<void()>>(r, "std::function");
  return 0;
}
