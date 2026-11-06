// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "ibmw_selfdev/coro/concepts.h"
#include "ibmw_selfdev/coro/export.h"
#include "ibmw_selfdev/coro/task.h"

namespace ibmw::selfdev::coro {

class AsyncScope;

namespace detail {

// Fire-and-forget coroutine: starts eagerly and frees its own frame.
struct DetachedDriver {
  struct promise_type {
    DetachedDriver get_return_object() const noexcept { return {}; }
    std::suspend_never initial_suspend() const noexcept { return {}; }
    std::suspend_never final_suspend() const noexcept { return {}; }
    void return_void() const noexcept {}
    // Same as unifex::async_scope::spawn: an escaping error is fatal.
    void unhandled_exception() const noexcept { std::terminate(); }
  };
};

template <class A>
DetachedDriver SpawnDriver(AsyncScope* scope, A a);

// spawn() runs a Task, or an rvalue that converts to one (On(s, a)), as is,
// without a DetachedDriver frame around it.
template <class A>
concept SpawnsAsTask = !std::is_lvalue_reference_v<A> && std::is_convertible_v<A&&, Task<AwaitValue<std::decay_t<A>>>>;

}  // namespace detail

/// Tracks spawned work and lets callers wait for all of it.
/// Names (spawn/complete/cleanup) match unifex::async_scope.
class AsyncScope : private detail::DetachedSink {
 public:
  AsyncScope() noexcept : DetachedSink{&AsyncScope::SinkDone} {}

  /// All spawned work must have finished (await complete() first). Checked
  /// in every build mode: running work would touch the destroyed scope, so
  /// destroying it with active work prints the reason and calls
  /// std::terminate() (the debug assert fires first in debug builds).
  ~AsyncScope() {
    if ((state_.load(std::memory_order_acquire) >> 1) != 0) {
      assert(false && "AsyncScope destroyed with active work");
      std::fputs("ibmw::selfdev::coro::AsyncScope destroyed with active work (await complete() first)\n", stderr);
      std::terminate();
    }
  }

  AsyncScope(const AsyncScope&) = delete;
  AsyncScope& operator=(const AsyncScope&) = delete;

  /// Starts `a` immediately on the calling thread (it runs until its first
  /// suspension). Returns false and leaves `a` untouched once the scope is
  /// closed. An exception escaping `a` calls std::terminate().
  template <class A>
    requires Awaitable<std::decay_t<A>>
  bool spawn(A&& a) {
    if (!TryAddTask()) return false;
    try {
      if constexpr (detail::SpawnsAsTask<A>) {
        SpawnTask(Task<AwaitValue<std::decay_t<A>>>(static_cast<A&&>(a)));
      } else {
        detail::SpawnDriver<std::decay_t<A>>(this, static_cast<A&&>(a));
      }
    } catch (...) {
      // Building the frame (allocating it, or copying, moving or converting
      // `a`) failed; the body never ran.
      TaskDone();
      throw;
    }
    return true;
  }

  class CompleteAwaiter {
   public:
    explicit CompleteAwaiter(AsyncScope* scope) noexcept : scope_(scope) {}

    // Closes the scope when the wait starts (as unifex does).
    bool await_ready() noexcept {
      scope_->Close();
      std::lock_guard<std::mutex> lock(scope_->mu_);
      return scope_->state_.load(std::memory_order_acquire) == 0;
    }

    // Publishes h to waiters_ under mu_; the last TaskDone() may resume it
    // (and destroy the frame holding *this) as soon as mu_ is released, so
    // only the local copy of scope_ is used.
    IBMW_SELFDEV_CORO_NOINLINE bool await_suspend(std::coroutine_handle<> h) {
      AsyncScope* const scope = scope_;
      std::lock_guard<std::mutex> lock(scope->mu_);
      if (scope->state_.load(std::memory_order_acquire) == 0) return false;
      scope->waiters_.push_back(h);
      return true;
    }

    void await_resume() const noexcept {}

   private:
    AsyncScope* scope_;
  };

  /// Awaitable that closes the scope and completes once every spawned task
  /// has finished. Waiters resume on the thread of the last finishing task.
  [[nodiscard]] CompleteAwaiter complete() noexcept { return CompleteAwaiter(this); }

  /// No cancellation support: same as complete().
  [[nodiscard]] CompleteAwaiter cleanup() noexcept { return CompleteAwaiter(this); }

  std::size_t ActiveCount() const noexcept {
    return static_cast<std::size_t>(state_.load(std::memory_order_acquire) >> 1);
  }

  bool Closed() const noexcept { return (state_.load(std::memory_order_acquire) & kOpen) == 0; }

 private:
  template <class A>
  friend detail::DetachedDriver detail::SpawnDriver(AsyncScope* scope, A a);

  // Runs `task` until its first suspension; when it finishes, it destroys
  // its frame and calls TaskDone().
  template <class T>
  void SpawnTask(Task<T> task) noexcept {
    auto h = std::move(task).Release();
    // Also enforced in release builds, as for co_await of an empty Task.
    if (!h) std::terminate();
    static_cast<detail::TaskPromiseBase&>(h.promise()).Detach(this);
    h.resume();
  }

  static void SinkDone(DetachedSink* sink) noexcept { static_cast<AsyncScope*>(sink)->TaskDone(); }

  static constexpr uint64_t kOpen = 1;
  static constexpr uint64_t kOne = 2;

  bool TryAddTask() noexcept {
    uint64_t s = state_.load(std::memory_order_relaxed);
    do {
      if ((s & kOpen) == 0) return false;
    } while (!state_.compare_exchange_weak(s, s + kOne, std::memory_order_acq_rel, std::memory_order_relaxed));
    return true;
  }

  void Close() noexcept { state_.fetch_and(~kOpen, std::memory_order_acq_rel); }

  // The transition to 0 (closed, no work) happens under mu_, and waiters
  // check for 0 under mu_ too. A waiter that sees 0 may destroy the scope at
  // once, so this function must not touch *this after unlocking.
  void TaskDone() noexcept {
    uint64_t s = state_.load(std::memory_order_relaxed);
    while (s - kOne != 0) {
      if (state_.compare_exchange_weak(s, s - kOne, std::memory_order_acq_rel, std::memory_order_relaxed)) return;
    }
    std::vector<std::coroutine_handle<>> waiters;
    {
      std::lock_guard<std::mutex> lock(mu_);
      state_.fetch_sub(kOne, std::memory_order_acq_rel);
      waiters.swap(waiters_);
    }
    for (auto h : waiters) h.resume();
  }

  // (active_count << 1) | open_bit
  std::atomic<uint64_t> state_{kOpen};
  std::mutex mu_;
  std::vector<std::coroutine_handle<>> waiters_;
};

// co_await operand: must not be an aggregate (GCC 11/12 destroy an
// aggregate-initialized co_await operand twice; see README.md, "Rules for
// awaiters").
static_assert(!std::is_aggregate_v<AsyncScope::CompleteAwaiter>);

namespace detail {

template <class A>
DetachedDriver SpawnDriver(AsyncScope* scope, A a) {
  {
    // Destroy the awaitable (task frame, captured arguments) before
    // reporting completion: a waiter may tear down what they refer to.
    A local(std::move(a));
    co_await std::move(local);
  }
  scope->TaskDone();
}

}  // namespace detail

}  // namespace ibmw::selfdev::coro
