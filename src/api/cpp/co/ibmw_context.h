// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#if defined(IBMW_EXECUTOR_USE_STDEXEC)

  #include <exec/timed_scheduler.hpp>
  #include <stdexec/execution.hpp>

  #include "cpp/context/details/thread_context.h"
  #include "cpp/executor/executor_bus.h"
  #include "cpp/util/function.h"

namespace ibmw::co {

// Executor
class IbmwExecutor {
 public:
  // OperationState
  template <typename Receiver>
    requires stdexec::receiver<Receiver>
  struct OperationState final {
    template <typename Receiver2>
      requires std::constructible_from<Receiver, Receiver2>
    OperationState(executor::ExecutorHandle executor_ref, Receiver2&& r)  //
        noexcept(std::is_nothrow_constructible_v<Receiver, Receiver2>)
        : executor_ref_(executor_ref), receiver_((Receiver2 &&) r) {}

    friend void tag_invoke(stdexec::start_t, OperationState& op) noexcept {
      op.executor_ref_.Dispatch([r{(Receiver &&) op.receiver_}, executor_ref{op.executor_ref_}]() mutable {
        auto previous_thread_ctx = ibmw::context::details::g_thread_ctx;
        ibmw::context::details::g_thread_ctx.exe = executor_ref;
        try {
          stdexec::set_value((Receiver &&) r);
        } catch (...) {
          stdexec::set_error((Receiver &&) r, std::current_exception());
        }
        ibmw::context::details::g_thread_ctx = std::move(previous_thread_ctx);
      });
    }

    executor::ExecutorHandle executor_ref_;
    Receiver receiver_;
  };

  // Sender
  class Task {
   public:
    using is_sender = void;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(std::exception_ptr)>;

    explicit Task(executor::ExecutorHandle executor_ref) noexcept
        : executor_ref_(executor_ref) {}

    template <class R>
    friend auto tag_invoke(stdexec::connect_t, const Task& self, R&& rec)  //
        noexcept(stdexec::__nothrow_constructible_from<stdexec::__decay_t<R>, R>) {
      return OperationState<std::remove_cvref_t<R>>(self.executor_ref_, (R &&) rec);
    }

    struct Env {
      executor::ExecutorHandle executor_ref_;

      template <class CPO>
      friend IbmwExecutor
      tag_invoke(stdexec::get_completion_scheduler_t<CPO>, const Env& self) noexcept {
        return IbmwExecutor(self.executor_ref_);
      }
    };

    friend Env tag_invoke(stdexec::get_env_t, const Task& self) noexcept {
      return Env{self.executor_ref_};
    }

   private:
    executor::ExecutorHandle executor_ref_;
  };

  // OperationState
  template <typename Receiver>
    requires stdexec::receiver<Receiver>
  struct ExecutorAtOperationState final {
    template <typename Receiver2>
      requires std::constructible_from<Receiver, Receiver2>
    ExecutorAtOperationState(
        executor::ExecutorHandle executor_ref,
        std::chrono::system_clock::time_point tp,
        Receiver2&& r)  //
        noexcept(std::is_nothrow_constructible_v<Receiver, Receiver2>)
        : executor_ref_(executor_ref), tp_(tp), receiver_((Receiver2 &&) r) {}

    friend void tag_invoke(stdexec::start_t, ExecutorAtOperationState& op) noexcept {
      op.executor_ref_.DispatchAt(op.tp_, [r{(Receiver &&) op.receiver_}, executor_ref{op.executor_ref_}]() mutable {
        auto previous_thread_ctx = ibmw::context::details::g_thread_ctx;
        ibmw::context::details::g_thread_ctx.exe = executor_ref;
        try {
          stdexec::set_value((Receiver &&) r);
        } catch (...) {
          stdexec::set_error((Receiver &&) r, std::current_exception());
        }
        ibmw::context::details::g_thread_ctx = std::move(previous_thread_ctx);
      });
    }

   private:
    executor::ExecutorHandle executor_ref_;
    std::chrono::system_clock::time_point tp_;
    Receiver receiver_;
  };

  // Sender
  class ExecutorAtTask {
   public:
    using is_sender = void;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(std::exception_ptr)>;

    ExecutorAtTask(
        executor::ExecutorHandle executor_ref,
        std::chrono::system_clock::time_point tp) noexcept
        : executor_ref_(executor_ref), tp_(tp) {}

    template <class R>
    friend auto tag_invoke(stdexec::connect_t, const ExecutorAtTask& self, R&& rec)  //
        noexcept(stdexec::__nothrow_constructible_from<stdexec::__decay_t<R>, R>) {
      return ExecutorAtOperationState<std::remove_cvref_t<R>>(self.executor_ref_, self.tp_, (R &&) rec);
    }

    struct Env {
      executor::ExecutorHandle executor_ref_;

      template <class CPO>
      friend IbmwExecutor
      tag_invoke(stdexec::get_completion_scheduler_t<CPO>, const Env& self) noexcept {
        return IbmwExecutor(self.executor_ref_);
      }
    };

    friend Env tag_invoke(stdexec::get_env_t, const ExecutorAtTask& self) noexcept {
      return Env{self.executor_ref_};
    }

   private:
    executor::ExecutorHandle executor_ref_;
    std::chrono::system_clock::time_point tp_;
  };

 public:
  explicit IbmwExecutor(executor::ExecutorHandle executor_ref) noexcept
      : executor_ref_(executor_ref) {}

  friend Task
  tag_invoke(stdexec::schedule_t, const IbmwExecutor& s) noexcept {
    return Task(s.executor_ref_);
  }

  friend std::chrono::system_clock::time_point
  tag_invoke(exec::now_t, const IbmwExecutor& s) noexcept {
    return s.executor_ref_.Now();
  }

  friend ExecutorAtTask
  tag_invoke(exec::schedule_after_t,
             const IbmwExecutor& s,
             std::chrono::nanoseconds dt) noexcept {
    return ExecutorAtTask(s.executor_ref_, dt + s.executor_ref_.Now());
  }

  friend ExecutorAtTask
  tag_invoke(exec::schedule_at_t,
             const IbmwExecutor& s,
             std::chrono::system_clock::time_point tp) noexcept {
    return ExecutorAtTask(s.executor_ref_, tp);
  }

  friend bool operator==(const IbmwExecutor& a, const IbmwExecutor& b) noexcept {
    return a.executor_ref_.NativeHandle() == b.executor_ref_.NativeHandle();
  }

  friend bool operator!=(const IbmwExecutor& a, const IbmwExecutor& b) noexcept {
    return a.executor_ref_.NativeHandle() != b.executor_ref_.NativeHandle();
  }

  explicit operator bool() const { return static_cast<bool>(executor_ref_); }

 private:
  executor::ExecutorHandle executor_ref_;
};

// Context
class IbmwContext {
 public:
  explicit IbmwContext(executor::ExecutorBusRef executor_bus_ref = {}) noexcept
      : executor_bus_ref_(executor_bus_ref) {}

  IbmwExecutor GetExecutor(std::string_view executor_name) {
    return IbmwExecutor(executor_bus_ref_.GetExecutor(executor_name));
  }

  explicit operator bool() const { return static_cast<bool>(executor_bus_ref_); }

 private:
  executor::ExecutorBusRef executor_bus_ref_;
};

}  // namespace ibmw::co

#elif defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)

  #include <chrono>
  #include <coroutine>
  #include <string_view>
  #include <type_traits>

  #include "ibmw_selfdev/coro/concepts.h"
  #include "ibmw_selfdev/coro/export.h"

  #include "cpp/context/details/thread_context.h"
  #include "cpp/executor/executor_bus.h"
  #include "cpp/util/function.h"

namespace ibmw::co {

namespace details {

// Resumes h on the executor's thread with g_thread_ctx.exe set. Only exe is
// set: ctx_ptr and active_rpc_context stay what the worker thread has, so
// code after co_await that needs the module context (GetCurrentContext())
// calls Context::LetMe() first. Coroutine bodies catch their own exceptions,
// so resume() does not throw.
inline auto MakeResumer(executor::ExecutorHandle executor_ref, std::coroutine_handle<> h) {
  return [executor_ref, h]() {
    // Restored even if resume() throws (a foreign coroutine type whose
    // unhandled_exception rethrows), so the worker keeps its own context.
    struct ThreadCtxRestore {
      decltype(ibmw::context::details::g_thread_ctx) previous = ibmw::context::details::g_thread_ctx;
      ~ThreadCtxRestore() { ibmw::context::details::g_thread_ctx = std::move(previous); }
    } restore;
    ibmw::context::details::g_thread_ctx.exe = executor_ref;
    h.resume();
  };
}

}  // namespace details

// Executor. A selfdev coro TimedScheduler: each schedule*() returns an
// awaiter that resumes the coroutine on the executor with g_thread_ctx.exe
// set, as the unifex branch's receivers do.
//
// Awaiter rule (see selfdev coro 接口设计.md): once Dispatch*() has handed the
// coroutine handle to the executor, the coroutine may run and be destroyed
// on another thread, so await_suspend touches neither the awaiter nor any
// other frame state afterwards, and is never inlined (Clang < 19 may place
// the locals of an inlined await_suspend, e.g. the temporary task wrapper
// destroyed after Dispatch() returns, in the coroutine frame).
class IbmwExecutor {
 public:
  // Awaiter
  class ScheduleAwaiter {
   public:
    explicit ScheduleAwaiter(executor::ExecutorHandle executor_ref) noexcept
        : executor_ref_(executor_ref) {}

    bool await_ready() const noexcept { return false; }
    IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) {
      executor_ref_.Dispatch(details::MakeResumer(executor_ref_, h));
    }
    void await_resume() const noexcept {}

   private:
    executor::ExecutorHandle executor_ref_;
  };

  // Awaiter
  class ScheduleAfterAwaiter {
   public:
    ScheduleAfterAwaiter(executor::ExecutorHandle executor_ref, const std::chrono::nanoseconds& dt) noexcept
        : executor_ref_(executor_ref), dt_(dt) {}

    bool await_ready() const noexcept { return false; }
    IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) {
      executor_ref_.DispatchAfter(dt_, details::MakeResumer(executor_ref_, h));
    }
    void await_resume() const noexcept {}

   private:
    executor::ExecutorHandle executor_ref_;
    std::chrono::nanoseconds dt_;
  };

  // Awaiter
  class ScheduleAtAwaiter {
   public:
    ScheduleAtAwaiter(executor::ExecutorHandle executor_ref,
                      const std::chrono::system_clock::time_point& tp) noexcept
        : executor_ref_(executor_ref), tp_(tp) {}

    bool await_ready() const noexcept { return false; }
    IBMW_SELFDEV_CORO_NOINLINE void await_suspend(std::coroutine_handle<> h) {
      executor_ref_.DispatchAt(tp_, details::MakeResumer(executor_ref_, h));
    }
    void await_resume() const noexcept {}

   private:
    executor::ExecutorHandle executor_ref_;
    std::chrono::system_clock::time_point tp_;
  };

  explicit IbmwExecutor(executor::ExecutorHandle executor_ref = {}) noexcept
      : executor_ref_(executor_ref) {}

  ScheduleAwaiter schedule() const noexcept { return ScheduleAwaiter(executor_ref_); }

  ScheduleAfterAwaiter schedule_after(const std::chrono::nanoseconds& dt) const noexcept {
    return ScheduleAfterAwaiter(executor_ref_, dt);
  }

  ScheduleAtAwaiter schedule_at(const std::chrono::system_clock::time_point& tp) const noexcept {
    return ScheduleAtAwaiter(executor_ref_, tp);
  }

  friend bool operator==(IbmwExecutor a, IbmwExecutor b) noexcept {
    return a.executor_ref_.NativeHandle() == b.executor_ref_.NativeHandle();
  }

  friend bool operator!=(IbmwExecutor a, IbmwExecutor b) noexcept {
    return a.executor_ref_.NativeHandle() != b.executor_ref_.NativeHandle();
  }

  explicit operator bool() const { return static_cast<bool>(executor_ref_); }

 private:
  executor::ExecutorHandle executor_ref_;
};

// Awaiters that are co_await operands must not be aggregates (GCC 11/12
// destroy an aggregate-initialized co_await operand twice; see selfdev coro
// 接口设计.md, "awaiter rules").
static_assert(!std::is_aggregate_v<IbmwExecutor::ScheduleAwaiter>);
static_assert(!std::is_aggregate_v<IbmwExecutor::ScheduleAfterAwaiter>);
static_assert(!std::is_aggregate_v<IbmwExecutor::ScheduleAtAwaiter>);

static_assert(ibmw::selfdev::coro::TimedScheduler<IbmwExecutor>);

// Context
class IbmwContext {
 public:
  explicit IbmwContext(executor::ExecutorBusRef executor_bus_ref = {}) noexcept
      : executor_bus_ref_(executor_bus_ref) {}

  IbmwExecutor GetExecutor(std::string_view executor_name) const {
    return IbmwExecutor(executor_bus_ref_.GetExecutor(executor_name));
  }

  explicit operator bool() const { return static_cast<bool>(executor_bus_ref_); }

 private:
  executor::ExecutorBusRef executor_bus_ref_;
};

}  // namespace ibmw::co

#else

  #include <memory>

  #include <unifex/execute.hpp>

  #include "cpp/context/details/thread_context.h"
  #include "cpp/executor/executor_bus.h"
  #include "cpp/util/function.h"

namespace ibmw::co {

// Executor
class IbmwExecutor {
 public:
  // OperationState
  template <typename Receiver>
  struct OperationState final {
    template <typename Receiver2>
      requires std::constructible_from<Receiver, Receiver2>
    explicit OperationState(executor::ExecutorHandle executor_ref, Receiver2&& r) noexcept(
        std::is_nothrow_constructible_v<Receiver, Receiver2>)
        : executor_ref_(executor_ref), receiver_((Receiver2 &&) r) {}

    void start() noexcept {
      auto receiver_ptr = std::make_unique<Receiver>(std::move(receiver_));
      executor_ref_.Dispatch([receiver_ptr{std::move(receiver_ptr)}, executor_ref{executor_ref_}]() mutable {
        auto previous_thread_ctx = ibmw::context::details::g_thread_ctx;
        ibmw::context::details::g_thread_ctx.exe = executor_ref;
        auto* receiver = receiver_ptr.release();
        try {
          unifex::set_value(std::move(*receiver));
        } catch (...) {
          unifex::set_error(std::move(*receiver), std::current_exception());
        }
        ibmw::context::details::g_thread_ctx = std::move(previous_thread_ctx);
      });
    }

    executor::ExecutorHandle executor_ref_;
    Receiver receiver_;
  };

  // Sender
  class Task {
   public:
    template <template <typename...> class Variant,
              template <typename...> class Tuple>
    using value_types = Variant<Tuple<>>;

    template <template <typename...> class Variant>
    using error_types = Variant<std::exception_ptr>;

    static constexpr bool sends_done = false;

    explicit Task(executor::ExecutorHandle executor_ref) noexcept
        : executor_ref_(executor_ref) {}

    template <typename Receiver>
    auto connect(Receiver&& receiver) {
      return OperationState<unifex::remove_cvref_t<Receiver>>(
          executor_ref_, (Receiver &&) receiver);
    }

   private:
    executor::ExecutorHandle executor_ref_;
  };

  // OperationState
  template <typename Receiver>
  struct ExecutorAfterOperationState final {
    template <typename Receiver2>
      requires std::constructible_from<Receiver, Receiver2>
    explicit ExecutorAfterOperationState(
        executor::ExecutorHandle executor_ref,
        const std::chrono::nanoseconds& dt,
        Receiver2&& r)  //
        noexcept(std::is_nothrow_constructible_v<Receiver, Receiver2>)
        : executor_ref_(executor_ref), dt_(dt), receiver_((Receiver2 &&) r) {}

    void start() noexcept {
      auto receiver_ptr = std::make_unique<Receiver>(std::move(receiver_));
      executor_ref_.DispatchAfter(dt_, [receiver_ptr{std::move(receiver_ptr)}, executor_ref{executor_ref_}]() mutable {
        auto previous_thread_ctx = ibmw::context::details::g_thread_ctx;
        ibmw::context::details::g_thread_ctx.exe = executor_ref;
        auto* receiver = receiver_ptr.release();
        try {
          unifex::set_value(std::move(*receiver));
        } catch (...) {
          unifex::set_error(std::move(*receiver), std::current_exception());
        }
        ibmw::context::details::g_thread_ctx = std::move(previous_thread_ctx);
      });
    }

   private:
    executor::ExecutorHandle executor_ref_;
    std::chrono::nanoseconds dt_;
    Receiver receiver_;
  };

  // Sender
  class ExecutorAfterTask {
   public:
    template <template <typename...> class Variant,
              template <typename...> class Tuple>
    using value_types = Variant<Tuple<>>;

    template <template <typename...> class Variant>
    using error_types = Variant<std::exception_ptr>;

    static constexpr bool sends_done = false;

    explicit ExecutorAfterTask(
        executor::ExecutorHandle executor_ref,
        const std::chrono::nanoseconds& dt) noexcept
        : executor_ref_(executor_ref), dt_(dt) {}

    template <typename Receiver>
    auto connect(Receiver&& receiver) {
      return ExecutorAfterOperationState<unifex::remove_cvref_t<Receiver>>(
          executor_ref_, dt_, (Receiver &&) receiver);
    }

   private:
    executor::ExecutorHandle executor_ref_;
    std::chrono::nanoseconds dt_;
  };

  // OperationState
  template <typename Receiver>
  struct ExecutorAtOperationState final {
    template <typename Receiver2>
      requires std::constructible_from<Receiver, Receiver2>
    explicit ExecutorAtOperationState(
        executor::ExecutorHandle executor_ref,
        const std::chrono::system_clock::time_point& tp,
        Receiver2&& r)  //
        noexcept(std::is_nothrow_constructible_v<Receiver, Receiver2>)
        : executor_ref_(executor_ref), tp_(tp), receiver_((Receiver2 &&) r) {}

    void start() noexcept {
      auto receiver_ptr = std::make_unique<Receiver>(std::move(receiver_));
      executor_ref_.DispatchAt(tp_, [receiver_ptr{std::move(receiver_ptr)}, executor_ref{executor_ref_}]() mutable {
        auto previous_thread_ctx = ibmw::context::details::g_thread_ctx;
        ibmw::context::details::g_thread_ctx.exe = executor_ref;
        auto* receiver = receiver_ptr.release();
        try {
          unifex::set_value(std::move(*receiver));
        } catch (...) {
          unifex::set_error(std::move(*receiver), std::current_exception());
        }
        ibmw::context::details::g_thread_ctx = std::move(previous_thread_ctx);
      });
    }

   private:
    executor::ExecutorHandle executor_ref_;
    std::chrono::system_clock::time_point tp_;
    Receiver receiver_;
  };

  // Sender
  class ExecutorAtTask {
   public:
    template <template <typename...> class Variant,
              template <typename...> class Tuple>
    using value_types = Variant<Tuple<>>;

    template <template <typename...> class Variant>
    using error_types = Variant<std::exception_ptr>;

    static constexpr bool sends_done = false;

    explicit ExecutorAtTask(
        executor::ExecutorHandle executor_ref,
        const std::chrono::system_clock::time_point& tp) noexcept
        : executor_ref_(executor_ref), tp_(tp) {}

    template <typename Receiver>
    auto connect(Receiver&& receiver) {
      return ExecutorAtOperationState<unifex::remove_cvref_t<Receiver>>(
          executor_ref_, tp_, (Receiver &&) receiver);
    }

   private:
    executor::ExecutorHandle executor_ref_;
    std::chrono::system_clock::time_point tp_;
  };

 public:
  explicit IbmwExecutor(executor::ExecutorHandle executor_ref = {}) noexcept
      : executor_ref_(executor_ref) {}

  Task schedule() const noexcept { return Task(executor_ref_); }

  ExecutorAfterTask schedule_after(const std::chrono::nanoseconds& dt) const noexcept {
    return ExecutorAfterTask(executor_ref_, dt);
  }

  ExecutorAtTask schedule_at(const std::chrono::system_clock::time_point& tp) const noexcept {
    return ExecutorAtTask(executor_ref_, tp);
  }

  friend bool operator==(IbmwExecutor a, IbmwExecutor b) noexcept {
    return a.executor_ref_.NativeHandle() == b.executor_ref_.NativeHandle();
  }

  friend bool operator!=(IbmwExecutor a, IbmwExecutor b) noexcept {
    return a.executor_ref_.NativeHandle() != b.executor_ref_.NativeHandle();
  }

  explicit operator bool() const { return static_cast<bool>(executor_ref_); }

 private:
  executor::ExecutorHandle executor_ref_;
};

// Context
class IbmwContext {
 public:
  explicit IbmwContext(executor::ExecutorBusRef executor_bus_ref = {}) noexcept
      : executor_bus_ref_(executor_bus_ref) {}

  IbmwExecutor GetExecutor(std::string_view executor_name) const {
    return IbmwExecutor(executor_bus_ref_.GetExecutor(executor_name));
  }

  explicit operator bool() const { return static_cast<bool>(executor_bus_ref_); }

 private:
  executor::ExecutorBusRef executor_bus_ref_;
};

}  // namespace ibmw::co

#endif
