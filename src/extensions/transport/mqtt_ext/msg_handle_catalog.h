// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
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
  ~MsgHandleCatalog() = default;

  MsgHandleCatalog(const MsgHandleCatalog&) = delete;
  MsgHandleCatalog& operator=(const MsgHandleCatalog&) = delete;

  template <typename... Args>
    requires std::constructible_from<MsgHandleFunc, Args...>
  void RegisterMsgHandle(std::string_view topic, Args&&... args) {
    msg_handle_map_.emplace(topic, std::forward<Args>(args)...);
  }

  // Takes ownership of the Paho message and topic buffer and frees them on
  // every path (TRANS-010), including drops, unknown topics, exceptions and
  // tasks discarded by the executor.
  void HandleServerMsg(std::string_view topic, char* topic_raw, MQTTAsync_message* message) const noexcept {
    // The guard outlives the message so that Shutdown() only returns once
    // every Paho buffer has been released.
    InflightGuard entry(this);
    HeldOnThisThread held;
    PahoMessage owned(message, topic_raw);
    if (shutdown_flag_.load()) [[unlikely]]
      return;

    try {
      MW_TRACE("Mqtt recv msg, topic: {}", topic);

      auto find_topic_itr = msg_handle_map_.find(topic);
      if (find_topic_itr == msg_handle_map_.end()) [[unlikely]] {
        MW_WARN("Unregisted topic: {}", topic);
        return;
      }

      if (executor_) {
        executor_.Dispatch(
            [guard = InflightGuard(this), handler = find_topic_itr->second, owned = std::move(owned)]() mutable {
              HeldOnThisThread held;
              RunHandler(handler, owned.message);
            });
        return;
      }
      RunHandler(find_topic_itr->second, owned.message);
    } catch (const std::exception& e) {
      MW_ERROR("Handle msg failed, topic: {}, exception info: {}", topic, e.what());
    } catch (...) {
      MW_ERROR("Handle msg failed, topic: {}, unknown exception.", topic);
    }
  }

  // Stops intake and waits until no handler runs any more, so the drivers
  // whose handlers capture `this` can be destroyed afterwards. In-flight
  // entries held by the calling thread (shutdown from inside a callback,
  // possibly through an inline executor) are not waited for.
  void Shutdown() {
    shutdown_flag_.store(true);
    std::unique_lock<std::mutex> lock(wait_mtx_);
    wait_cv_.wait(lock, [this]() { return active_tasks_.load() <= tl_held_; });
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
    explicit InflightGuard(const MsgHandleCatalog* owner) noexcept : owner_(owner) { owner_->active_tasks_.fetch_add(1); }
    InflightGuard(InflightGuard&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
    InflightGuard& operator=(InflightGuard&&) = delete;
    ~InflightGuard() {
      if (owner_ == nullptr) return;
      owner_->active_tasks_.fetch_sub(1);
      if (owner_->shutdown_flag_.load()) [[unlikely]] {
        std::lock_guard<std::mutex> lock(owner_->wait_mtx_);
        owner_->wait_cv_.notify_all();
      }
    }

   private:
    const MsgHandleCatalog* owner_;
  };

  // Marks one active_tasks_ count as held by the current thread.
  struct HeldOnThisThread {
    HeldOnThisThread() noexcept { ++tl_held_; }
    ~HeldOnThisThread() { --tl_held_; }
    HeldOnThisThread(const HeldOnThisThread&) = delete;
    HeldOnThisThread& operator=(const HeldOnThisThread&) = delete;
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

  static inline thread_local int tl_held_ = 0;

  std::atomic_bool shutdown_flag_ = false;

  mutable std::atomic_int active_tasks_ = 0;
  mutable std::mutex wait_mtx_;
  mutable std::condition_variable wait_cv_;

  using UriMsgHandleMap = std::unordered_map<std::string, MsgHandleFunc, ibmw::common::util::StringHash, std::equal_to<>>;
  UriMsgHandleMap msg_handle_map_;

  std::function<executor::ExecutorHandle(std::string_view)> get_executor_func_;
  mutable ibmw::executor::ExecutorHandle executor_;
};

}  // namespace ibmw::extensions::mqtt_extension
