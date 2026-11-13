// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "ibmw_selfdev/coro/concepts.h"
#include "ibmw_selfdev/coro/export.h"

namespace ibmw::selfdev::coro {

namespace detail {

template <class V>
struct SyncWaitState {
  std::mutex mu;
  std::condition_variable cv;
  bool done = false;
  std::optional<V> value;
  std::exception_ptr error;
};

template <class V>
struct SyncWaitDriver {
  struct promise_type {
    SyncWaitState<V>* state = nullptr;

    SyncWaitDriver get_return_object() noexcept {
      return SyncWaitDriver{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() const noexcept { return {}; }

    struct FinalAwaiter {
      FinalAwaiter() noexcept = default;
      bool await_ready() const noexcept { return false; }
      // Runs after the frame is suspended, so SyncWait may destroy it as
      // soon as it observes `done`: only the local `st` is used. Notify
      // while holding the lock: the state lives on SyncWait's stack.
      IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<promise_type> h) const noexcept {
        SyncWaitState<V>* st = h.promise().state;
        std::lock_guard<std::mutex> lock(st->mu);
        st->done = true;
        st->cv.notify_all();
      }
      void await_resume() const noexcept {}
    };
    FinalAwaiter final_suspend() const noexcept { return {}; }

    void return_void() const noexcept {}
    // The body catches everything; reaching here means storing the error failed.
    void unhandled_exception() const noexcept { std::terminate(); }
  };

  std::coroutine_handle<promise_type> handle;
};

template <class V, class A>
SyncWaitDriver<V> SyncWaitBody(SyncWaitState<V>& st, A a) {
  try {
    if constexpr (std::is_same_v<V, Unit> && std::is_void_v<AwaitValue<A>>) {
      co_await std::move(a);
      st.value.emplace();
    } else {
      st.value.emplace(co_await std::move(a));
    }
  } catch (...) {
    st.error = std::current_exception();
  }
}

struct SyncWaitFn {
  template <class A>
    requires Awaitable<std::decay_t<A>>
  auto operator()(A&& a) const {
    using R = AwaitValue<std::decay_t<A>>;
    using V = std::conditional_t<std::is_void_v<R>, Unit, R>;

    SyncWaitState<V> st;
    auto driver = SyncWaitBody<V, std::decay_t<A>>(st, static_cast<A&&>(a));
    driver.handle.promise().state = &st;
    driver.handle.resume();
    {
      std::unique_lock<std::mutex> lock(st.mu);
      st.cv.wait(lock, [&] { return st.done; });
    }
    driver.handle.destroy();
    if (st.error) std::rethrow_exception(st.error);
    return std::move(st.value);
  }
};

}  // namespace detail

/// Blocks the calling thread until `a` completes. Returns std::optional<R>
/// (Unit for void); exceptions from `a` are rethrown. The calling thread does
/// not run an event loop, so `a` must be able to complete elsewhere.
inline constexpr detail::SyncWaitFn SyncWait{};

}  // namespace ibmw::selfdev::coro
