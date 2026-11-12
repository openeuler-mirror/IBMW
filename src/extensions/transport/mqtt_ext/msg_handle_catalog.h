// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "MQTTAsync.h"
#include "cpp/executor/executor.h"
#include "base/string_util.h"

#include "mqtt_ext/global.h"

namespace ibmw::extensions::mqtt_extension {

class MsgHandleCatalog {
 public:
  using MsgHandleFunc = std::function<void(MQTTAsync_message* message)>;

  MsgHandleCatalog() = default;
  ~MsgHandleCatalog() {
    // The catalog holds one count itself; whoever drops the last count frees
    // the state, so a task the executor still holds (shutdown skipped the
    // drain) can release its count after the catalog is gone.
    state_->shutdown.store(true);
    if (state_->active.fetch_sub(1, std::memory_order_acq_rel) == 1) delete state_;
  }

  MsgHandleCatalog(const MsgHandleCatalog&) = delete;
  MsgHandleCatalog& operator=(const MsgHandleCatalog&) = delete;

  // Must be called before the client can deliver messages: the map is not
  // synchronized and is read-only once sealed.
  template <typename... Args>
    requires std::constructible_from<MsgHandleFunc, Args...>
  void RegisterMsgHandle(std::string_view topic, Args&&... args) {
    MW_REQUIRE(!sealed_, "Mqtt msg handle for '{}' registered after the client was connected.", topic);
    msg_handle_map_.emplace(topic, std::forward<Args>(args)...);
  }

  // Called before the first connect: no handler may be added afterwards.
  void Seal() noexcept { sealed_ = true; }
  bool IsSealed() const noexcept { return sealed_; }

  // Takes ownership of the Paho message and topic buffer and frees them on
  // every path (TRANS-010), including drops, unknown topics, exceptions and
  // tasks discarded by the executor.
  void HandleServerMsg(std::string_view topic, char* topic_raw, MQTTAsync_message* message) const noexcept {
    // One in-flight count per message, released after the Paho buffers.
    // Pairs with Shutdown(): either the flag is seen here or the count there.
    InflightGuard guard(state_);
    PahoMessage owned(message, topic_raw);
    if (state_->shutdown.load()) [[unlikely]]
      return;

    try {
      MW_TRACE("Mqtt recv msg, topic: {}", topic);

      auto find_topic_itr = msg_handle_map_.find(topic);
      if (find_topic_itr == msg_handle_map_.end()) [[unlikely]] {
        MW_WARN_INTERVAL(1000, "Unregisted topic: {}", topic);
        return;
      }

      if (executor_) {
        executor_.Dispatch(
            [guard = std::move(guard), handler = find_topic_itr->second, owned = std::move(owned)]() mutable {
              // Queued behind a shutdown: the handler's owner may be gone.
              if (guard.State()->shutdown.load(std::memory_order_acquire)) [[unlikely]]
                return;
              RunningOnThisThread running(guard.State());
              RunHandler(handler, owned.message);
            });
        return;
      }
      RunningOnThisThread running(state_);
      RunHandler(find_topic_itr->second, owned.message);
    } catch (const std::exception& e) {
      // The topic buffer may already be released with a failed task.
      MW_ERROR("Handle msg failed, exception info: {}", e.what());
    } catch (...) {
      MW_ERROR("Handle msg failed, unknown exception.");
    }
  }

  // Stops intake and waits until no handler runs any more, so the drivers
  // whose handlers capture `this` can be destroyed afterwards. A handler of
  // this catalog running on the calling thread (shutdown from inside a
  // callback) is not waited for. Called from the callback executor, queued
  // tasks cannot run before this returns: they are dropped instead of waited
  // for (as in InProcessTransport::DrainInflight).
  void Shutdown() {
    state_->shutdown.store(true);
    const int self = tl_running_ == state_ ? 1 : 0;
    if (executor_ && executor_.IsInCurrentExecutor()) {
      MW_WARN("Mqtt msg catalog shut down from its callback executor; running handlers are not drained.");
      return;
    }
    // Pairs with the seq_cst increment in HandleServerMsg(). One count is
    // the catalog's own.
    const int idle = 1 + self;
    if (state_->active.load() <= idle) return;
    auto next_warn = std::chrono::steady_clock::now() + kSlowShutdownWarn;
    while (state_->active.load(std::memory_order_acquire) > idle) {
      if (std::chrono::steady_clock::now() >= next_warn) {
        MW_WARN("Mqtt msg catalog shutdown is waiting for {} message handler(s).",
                state_->active.load(std::memory_order_relaxed) - idle);
        next_warn += kSlowShutdownWarn;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void RegisterGetExecutorFunc(
      const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func) {
    get_executor_func_ = get_executor_func;
  }

  void InitExecutor(std::string_view executor_name) {
    executor_ = get_executor_func_(executor_name);
    MW_REQUIRE(executor_, "Can not get executor {}.", executor_name);
  }

 private:
  static constexpr auto kSlowShutdownWarn = std::chrono::seconds(5);

  // Separately allocated so that a task released after the catalog is gone
  // still has valid memory to touch.
  struct alignas(64) SharedState {
    std::atomic<int> active{1};  // the catalog's own count
    std::atomic<bool> shutdown{false};
  };

  struct PahoMessage {
    PahoMessage(MQTTAsync_message* m, char* t) noexcept : message(m), topic(t) {}
    PahoMessage(PahoMessage&& other) noexcept
        : message(std::exchange(other.message, nullptr)), topic(std::exchange(other.topic, nullptr)) {}
    PahoMessage& operator=(PahoMessage&&) = delete;
    ~PahoMessage() {
      if (message) MQTTAsync_freeMessage(&message);
      if (topic) MQTTAsync_free(topic);
    }
    MQTTAsync_message* message;
    char* topic;
  };

  class InflightGuard {
   public:
    explicit InflightGuard(SharedState* state) noexcept : state_(state) { state_->active.fetch_add(1); }
    InflightGuard(InflightGuard&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}
    InflightGuard& operator=(InflightGuard&&) = delete;
    // The decrement is the last access: Shutdown() may return right after it.
    ~InflightGuard() {
      if (state_ && state_->active.fetch_sub(1, std::memory_order_acq_rel) == 1) delete state_;
    }
    SharedState* State() const noexcept { return state_; }

   private:
    SharedState* state_;
  };

  // Marks the current thread as running a handler of this catalog.
  class RunningOnThisThread {
   public:
    explicit RunningOnThisThread(const SharedState* state) noexcept : prev_(std::exchange(tl_running_, state)) {}
    ~RunningOnThisThread() { tl_running_ = prev_; }
    RunningOnThisThread(const RunningOnThisThread&) = delete;
    RunningOnThisThread& operator=(const RunningOnThisThread&) = delete;

   private:
    const SharedState* prev_;
  };

  static void RunHandler(const MsgHandleFunc& handler, MQTTAsync_message* message) noexcept {
    try {
      handler(message);
    } catch (const std::exception& e) {
      MW_ERROR("Mqtt msg handler failed: {}", e.what());
    } catch (...) {
      MW_ERROR("Mqtt msg handler failed with an unknown exception.");
    }
  }

  static inline thread_local const SharedState* tl_running_ = nullptr;

  SharedState* const state_ = new SharedState();
  bool sealed_ = false;

  using UriMsgHandleMap = std::unordered_map<std::string, MsgHandleFunc, ibmw::common::util::StringHash, std::equal_to<>>;
  UriMsgHandleMap msg_handle_map_;

  std::function<executor::ExecutorHandle(std::string_view)> get_executor_func_;
  mutable ibmw::executor::ExecutorHandle executor_;
};

}  // namespace ibmw::extensions::mqtt_extension
