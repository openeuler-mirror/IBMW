// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cassert>
#include <concepts>
#include <coroutine>
#include <exception>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "ibmw_selfdev/coro/export.h"

namespace ibmw::selfdev::coro {

template <class T = void>
class Task;

class AsyncScope;

namespace detail {

#if defined(_MSC_VER) && !defined(__clang__)
  #define IBMW_SELFDEV_CORO_OPAQUE __declspec(noinline)
  #define IBMW_SELFDEV_CORO_BARRIER() ((void)0)
#else
  #define IBMW_SELFDEV_CORO_OPAQUE [[gnu::noinline]]
  #define IBMW_SELFDEV_CORO_BARRIER() asm volatile("" ::: "memory")
#endif

// Thread identity that survives coroutine suspension. Clang treats
// pthread_self() (behind std::this_thread::get_id()) as a const function and
// may reuse a value computed before a suspension point after the coroutine
// resumed on another thread. An opaque, non-inlined call with a compiler
// barrier is never merged or hoisted. Stateless, so a copy per DSO is fine.
IBMW_SELFDEV_CORO_OPAQUE inline std::thread::id CurrentThreadId() noexcept {
  IBMW_SELFDEV_CORO_BARRIER();
  return std::this_thread::get_id();
}

// The innermost TaskPromiseBase::Start() running on this thread. Lives on
// Start()'s stack.
struct StartRecord {
  const void* promise;
  bool done_inline;
};

// This thread's slot for the innermost StartRecord. Defined out of line in
// libibmw_selfdev_coro (src/task.cc), so the process has exactly one slot per
// thread whatever the DSO layout: an inline function-local thread_local was
// duplicated per DSO under RTLD_DEEPBIND (Clang), -Bsymbolic or version
// scripts, which turned synchronous completions across DSOs into nested
// resumes (stack overflow in long loops). Exposed for tests and diagnostics.
IBMW_SELFDEV_CORO_API StartRecord*& CurrentStartRecord() noexcept;

// Told when a detached task (TaskPromiseBase::Detach()) has finished. A
// function pointer rather than a virtual call keeps it a plain aggregate that
// AsyncScope embeds as a base.
struct DetachedSink {
  void (*on_done)(DetachedSink* self) noexcept;
};

class TaskPromiseBase {
 public:
  // How the parent is resumed when the task finishes:
  //  - Inside the Start() call that launched it, on the same thread (it
  //    completed synchronously): the final awaiter marks Start()'s stack
  //    record and returns noop; Start() returns false and the parent goes on
  //    inline. A loop of synchronous awaits therefore never nests resume()
  //    calls (plain symmetric transfer relies on a tail call, which GCC does
  //    not emit at -O0).
  //  - Anywhere else (another thread, or later on the same thread): the final
  //    awaiter transfers to the parent at once, on the completing thread, as
  //    unifex does. Start() may still be returning on its own thread; it
  //    touches only its stack and the thread_local after resume(), never the
  //    frames, so nothing waits and no atomics are needed. Visibility of
  //    continuation_ to the completing thread comes from whatever handed the
  //    task over (executor queue, RPC callback, ...).
  //  - Detached (AsyncScope::spawn, see Detach()): there is no parent; the
  //    final awaiter destroys the frame, then tells the sink.
  struct FinalAwaiter {
    FinalAwaiter() noexcept = default;
    bool await_ready() const noexcept { return false; }

    template <class Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
      // Only a detached task has no continuation: the awaited path pays no
      // extra load or branch beyond this null test.
      const std::coroutine_handle<> next = static_cast<TaskPromiseBase&>(h.promise()).Finish();
      if (next) return next;
      FinishDetached(h);
      return std::noop_coroutine();
    }

    void await_resume() const noexcept {}

   private:
    // Out of line so that await_suspend above stays small where it is
    // inlined into every Task body.
    template <class Promise>
    IBMW_SELFDEV_CORO_NOINLINE static void FinishDetached(std::coroutine_handle<Promise> h) noexcept {
      Promise& p = h.promise();
      DetachedSink* const sink = static_cast<TaskPromiseBase&>(p).detached_;
      // A null continuation without Detach() is a frame resumed by hand.
      if (sink == nullptr) std::terminate();
      // Nobody takes the result. Same as unifex::async_scope::spawn: an
      // escaping error is fatal. Rethrown inside a handler so that the
      // terminate handler can report it.
      if (p.Failed()) {
        try {
          p.RethrowError();
        } catch (...) {
          std::terminate();
        }
      }
      // The frame goes first: whoever on_done() wakes may tear down what it
      // refers to.
      h.destroy();
      sink->on_done(sink);
    }
  };

  std::suspend_always initial_suspend() const noexcept { return {}; }
  FinalAwaiter final_suspend() const noexcept { return {}; }

  /// Runs the task until it completes or suspends. Returns true if the
  /// caller must suspend (`awaiting` will be resumed on completion; it may
  /// already be running elsewhere when this returns), false if the task
  /// already finished synchronously on this thread.
  /// Out of line (src/task.cc): the record lives on Start()'s own stack,
  /// never in a coroutine frame, even when the awaiting coroutine is inlined
  /// and split by the compiler.
  IBMW_SELFDEV_CORO_API IBMW_SELFDEV_CORO_NOPLT bool Start(std::coroutine_handle<> self, std::coroutine_handle<> awaiting) noexcept;

 private:
  friend class coro::AsyncScope;

  // For a task run without an awaiter (AsyncScope::spawn), before it is
  // first resumed: when it finishes, its frame is destroyed, then
  // sink->on_done(sink) is called on the finishing thread. An exception
  // escaping the task calls std::terminate().
  void Detach(DetachedSink* sink) noexcept {
    continuation_ = {};
    detached_ = sink;
  }

  // Called by FinalAwaiter. Inside the Start() that launched this task on
  // this thread: marks its record and returns noop. Otherwise returns the
  // continuation (symmetric transfer on the completing thread), which is
  // null only for a detached task.
  IBMW_SELFDEV_CORO_API IBMW_SELFDEV_CORO_NOPLT std::coroutine_handle<> Finish() noexcept;

  std::coroutine_handle<> continuation_ = std::noop_coroutine();
  // Record of the Start() that launched this task; dangling once that
  // Start() returned, and then only compared, never dereferenced.
  const StartRecord* start_ = nullptr;
  // Set by Detach(); the frame then has no awaiter.
  DetachedSink* detached_ = nullptr;
};

template <class T>
class TaskPromise final : public TaskPromiseBase {
 public:
  Task<T> get_return_object() noexcept;

  template <class U = T>
    requires std::convertible_to<U&&, T>
  void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
    result_.template emplace<1>(std::forward<U>(value));
  }

  void unhandled_exception() noexcept { result_.template emplace<2>(std::current_exception()); }

  bool Failed() const noexcept { return result_.index() == 2; }

  [[noreturn]] void RethrowError() const { std::rethrow_exception(std::get<2>(result_)); }

  T TakeResult() {
    if (result_.index() == 2) std::rethrow_exception(std::get<2>(result_));
    assert(result_.index() == 1 && "Task resumed without a result");
    return std::move(std::get<1>(result_));
  }

 private:
  std::variant<std::monostate, T, std::exception_ptr> result_;
};

template <>
class TaskPromise<void> final : public TaskPromiseBase {
 public:
  Task<void> get_return_object() noexcept;

  void return_void() const noexcept {}

  void unhandled_exception() noexcept { error_ = std::current_exception(); }

  bool Failed() const noexcept { return static_cast<bool>(error_); }

  [[noreturn]] void RethrowError() const { std::rethrow_exception(error_); }

  void TakeResult() {
    if (error_) std::rethrow_exception(error_);
  }

 private:
  std::exception_ptr error_;
};

}  // namespace detail

/// Lazily started, move-only coroutine. Starts when awaited (or spawned /
/// SyncWait'ed) and resumes its awaiter on whatever
/// thread it completes on (not scheduler affine). Exceptions propagate to the
/// awaiter. Any awaitable may be co_awaited inside (no await_transform).
template <class T>
class [[nodiscard]] Task {
  static_assert(!std::is_reference_v<T>, "Task<T&> is not supported");

 public:
  using promise_type = detail::TaskPromise<T>;
  using value_type = T;

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  /// Destroys the frame if the task was never started or has finished.
  ~Task() {
    if (handle_) handle_.destroy();
  }

  bool Valid() const noexcept { return static_cast<bool>(handle_); }

  /// Takes ownership of the frame; the Task becomes invalid.
  auto operator co_await() && noexcept {
    // Also enforced in release builds: resuming a null frame is UB.
    if (!handle_) std::terminate();
    return Awaiter{std::exchange(handle_, {})};
  }

 private:
  friend class detail::TaskPromise<T>;
  friend class AsyncScope;
  using Handle = std::coroutine_handle<promise_type>;

  // Takes ownership of the frame; the Task becomes invalid.
  Handle Release() && noexcept { return std::exchange(handle_, {}); }

  class Awaiter {
   public:
    explicit Awaiter(Handle h) noexcept : handle_(h) {}
    Awaiter(Awaiter&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Awaiter& operator=(Awaiter&&) = delete;
    ~Awaiter() {
      if (handle_) handle_.destroy();
    }

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> awaiting) noexcept { return handle_.promise().Start(handle_, awaiting); }

    T await_resume() { return handle_.promise().TakeResult(); }

   private:
    Handle handle_;
  };

  explicit Task(Handle h) noexcept : handle_(h) {}

  Handle handle_;
};

namespace detail {

template <class T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
  return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
  return Task<void>(std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
}

}  // namespace detail

}  // namespace ibmw::selfdev::coro
