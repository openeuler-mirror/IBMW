// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>

#include "cpp/executor/executor.h"

#include "tbb/concurrent_hash_map.h"

namespace ibmw::runtime::core::util {

// Start of a request id sequence: not predictable from outside, so an
// injected answer has to guess it (ids on the wire are 32-bit).
inline uint32_t RandomReqIdStart() noexcept {
  try {
    return std::random_device{}();
  } catch (...) {
    return static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  }
}

template <typename MsgRecorder>
class RpcClientTool {
 public:
  using TimeoutHandle = std::function<void(MsgRecorder&&)>;

 public:
  RpcClientTool() : state_(std::make_shared<State>()) {}
  ~RpcClientTool() = default;

  RpcClientTool(const RpcClientTool&) = delete;
  RpcClientTool& operator=(const RpcClientTool&) = delete;

  void RegisterTimeoutExecutor(ibmw::executor::ExecutorHandle timeout_executor) {
    if (!timeout_executor.SupportsTimedDispatch())
      throw std::runtime_error("Timeout executor do not support timer schedule.");

    timeout_executor_ = timeout_executor;
  }

  void RegisterTimeoutHandle(const TimeoutHandle& timeout_handle) {
    state_->timeout_handle = timeout_handle;
  }

  bool Record(uint32_t req_id, std::chrono::nanoseconds timeout, MsgRecorder&& msg_recorder) {
    bool ret = state_->client_msg_recorder_map.emplace(req_id, std::move(msg_recorder));

    if (!ret) [[unlikely]]
      return false;

    if (timeout_executor_) {
      // The timeout task only holds a weak reference to the record map: it
      // is a no-op once the tool is destroyed, and a task that already
      // locked the state keeps it alive while it runs (no check-then-use
      // race with the destructor).
      std::weak_ptr<State> weak_state = state_;
      try {
        timeout_executor_.DispatchAfter(timeout, [weak_state = std::move(weak_state), req_id]() {
          auto state = weak_state.lock();
          if (!state) [[unlikely]]
            return;

          // Pairs with AbandonAll(): either it sees this task or the task sees
          // the closed flag (the counter lives on the timeout path only).
          TimeoutTaskGuard running(*state);
          if (state->closed.load()) [[unlikely]]
            return;

          auto msg_recorder_op = state->Take(req_id);
          if (msg_recorder_op && state->timeout_handle) [[unlikely]] {
            InTimeoutHandle in_handle(*state);
            state->timeout_handle(std::move(*msg_recorder_op));
          }
        });
      } catch (...) {
        // A record without its timeout would never end: give it back, the
        // caller reports the call as failed.
        state_->Take(req_id);
        return false;
      }
    }

    return true;
  }

  // For a failed Record(): the id is taken by a call still pending after a wrap
  // of the 32-bit ids, so try the next ones. Out of line, Record() stays the only
  // cost of a normal call. `req_id` is the id recorded under on success.
  template <typename NextId>
  [[gnu::noinline, gnu::cold]] bool RetryRecord(
      uint32_t& req_id, std::chrono::nanoseconds timeout, const MsgRecorder& msg_recorder, NextId&& next_id) {
    for (int retry = 0; retry < 8; ++retry) {
      req_id = next_id();
      if (Record(req_id, timeout, MsgRecorder(msg_recorder))) return true;
    }
    return false;
  }

  std::optional<MsgRecorder> GetRecord(uint32_t req_id) { return state_->Take(req_id); }

  // Takes the record only if `matches(record)`: an answer that does not
  // belong to the call (e.g. arrived for another function) leaves it pending.
  template <typename Pred>
  std::optional<MsgRecorder> GetRecordIf(uint32_t req_id, Pred&& matches) {
    return state_->TakeIf(req_id, std::forward<Pred>(matches));
  }

  // Hands every remaining record to `handle` exactly once, so no call is
  // dropped without a result; an exception from `handle` only affects its
  // own record. Later timeouts are no-ops and timeout tasks still running
  // are waited for; called from this tool's timeout handle (a timeout that
  // shuts the driver down) it does not wait for that task itself. It must
  // not be called from another timeout task of this tool.
  //
  // Known limitation: concurrent Record()/GetRecord() calls are not
  // synchronized with this (the map is iterated). Call it from the driver
  // destructor; the service dispatcher does not yet drain in-flight Call()s
  // before destroying drivers (pre-existing, tracked separately).
  void AbandonAll(const TimeoutHandle& handle) noexcept {
    state_->closed.store(true);
    const int self = InTimeoutHandle::Current() == state_.get() ? 1 : 0;
    while (state_->running_timeout_tasks.load() != self) std::this_thread::yield();

    for (auto& item : state_->client_msg_recorder_map) {
      try {
        handle(std::move(item.second));
      } catch (...) {
      }
    }
    state_->client_msg_recorder_map.clear();
  }

 private:
  using ClientMsgRecorderMap = tbb::concurrent_hash_map<uint32_t, MsgRecorder>;

  struct State {
    TimeoutHandle timeout_handle;
    ClientMsgRecorderMap client_msg_recorder_map;
    std::atomic<bool> closed{false};
    std::atomic<int> running_timeout_tasks{0};

    std::optional<MsgRecorder> Take(uint32_t req_id) {
      typename ClientMsgRecorderMap::accessor ac;
      bool find_ret = client_msg_recorder_map.find(ac, req_id);

      if (!find_ret) [[unlikely]]
        return {};

      auto result = std::make_optional<MsgRecorder>(std::move(ac->second));

      client_msg_recorder_map.erase(ac);

      return result;
    }

    // Take() for records that satisfy `matches` only (kept separate so that
    // the plain Take() stays as it is).
    template <typename Pred>
    std::optional<MsgRecorder> TakeIf(uint32_t req_id, Pred&& matches) {
      typename ClientMsgRecorderMap::accessor ac;
      bool find_ret = client_msg_recorder_map.find(ac, req_id);

      if (!find_ret || !matches(std::as_const(ac->second))) [[unlikely]]
        return {};

      auto result = std::make_optional<MsgRecorder>(std::move(ac->second));

      client_msg_recorder_map.erase(ac);

      return result;
    }
  };

  class TimeoutTaskGuard {
   public:
    explicit TimeoutTaskGuard(State& state) noexcept : state_(state) { state_.running_timeout_tasks.fetch_add(1); }
    ~TimeoutTaskGuard() { state_.running_timeout_tasks.fetch_sub(1, std::memory_order_release); }
    TimeoutTaskGuard(const TimeoutTaskGuard&) = delete;
    TimeoutTaskGuard& operator=(const TimeoutTaskGuard&) = delete;

   private:
    State& state_;
  };

  // Marks the thread while it runs a timeout handle; only a timeout that
  // fires pays for it.
  class InTimeoutHandle {
   public:
    explicit InTimeoutHandle(const State& state) noexcept : outer_(current_) { current_ = &state; }
    ~InTimeoutHandle() { current_ = outer_; }
    InTimeoutHandle(const InTimeoutHandle&) = delete;
    InTimeoutHandle& operator=(const InTimeoutHandle&) = delete;

    static const State* Current() noexcept { return current_; }

   private:
    static inline thread_local const State* current_ = nullptr;
    const State* outer_;
  };

  std::shared_ptr<State> state_;

  ibmw::executor::ExecutorHandle timeout_executor_;
};

}  // namespace ibmw::runtime::core::util
