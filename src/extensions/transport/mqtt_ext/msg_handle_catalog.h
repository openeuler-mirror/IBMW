// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_map>

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

  void HandleServerMsg(std::string_view topic, MQTTAsync_message* message) const {
    if (shutdown_flag_.load()) [[unlikely]]
      return;

    MW_TRACE("Mqtt recv msg, topic: {}", topic);

    try {
      auto find_topic_itr = msg_handle_map_.find(topic);
      if (find_topic_itr == msg_handle_map_.end()) [[unlikely]] {
        MW_WARN("Unregisted topic: {}", topic);
        return;
      }

      if (executor_) {
        auto& handler = find_topic_itr->second;

        active_tasks_++;

        // Capture the handler by value [handler]
        executor_.Dispatch([this, handler, message, topic]() mutable {
          handler(message);
          MQTTAsync_freeMessage(&message);
          MQTTAsync_free(const_cast<char*>(topic.data()));

          if (--active_tasks_ == 0) {
            std::lock_guard<std::mutex> lock(wait_mtx_);
            wait_cv_.notify_all();
          }
        });
        return;
      }
      find_topic_itr->second(message);

    } catch (const std::exception& e) {
      MW_ERROR("Handle msg failed, topic: {}, exception info: {}", topic, e.what());
      return;
    }
  }

  void Shutdown() {
    if (std::atomic_exchange(&shutdown_flag_, true)) return;
  }

  void RegisterGetExecutorFunc(
      const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func) {
    get_executor_func_ = get_executor_func;
  }

  void InitExecutor(std::string_view executor_name) {
    executor_ = get_executor_func_(executor_name);
    MW_REQUIRE(executor_, "Can not get executor {}.", executor_name);
  }

  void WaitForAllTasks() const {
    if (active_tasks_.load() == 0) return;

    std::unique_lock<std::mutex> lock(wait_mtx_);
    wait_cv_.wait(lock, [this]() {
      return active_tasks_.load() == 0;
    });
  }

 private:
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
