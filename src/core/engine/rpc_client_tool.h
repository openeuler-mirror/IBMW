// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>

#include "cpp/executor/executor.h"

#include "tbb/concurrent_hash_map.h"

namespace ibmw::runtime::core::util {

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
      timeout_executor_.DispatchAfter(timeout, [weak_state = std::move(weak_state), req_id]() {
        auto state = weak_state.lock();
        if (!state) [[unlikely]]
          return;

        auto msg_recorder_op = state->Take(req_id);
        if (msg_recorder_op && state->timeout_handle) [[unlikely]]
          state->timeout_handle(std::move(*msg_recorder_op));
      });
    }

    return true;
  }

  std::optional<MsgRecorder> GetRecord(uint32_t req_id) { return state_->Take(req_id); }

 private:
  using ClientMsgRecorderMap = tbb::concurrent_hash_map<uint32_t, MsgRecorder>;

  struct State {
    TimeoutHandle timeout_handle;
    ClientMsgRecorderMap client_msg_recorder_map;

    std::optional<MsgRecorder> Take(uint32_t req_id) {
      typename ClientMsgRecorderMap::accessor ac;
      bool find_ret = client_msg_recorder_map.find(ac, req_id);

      if (!find_ret) [[unlikely]]
        return {};

      auto result = std::make_optional<MsgRecorder>(std::move(ac->second));

      client_msg_recorder_map.erase(ac);

      return result;
    }
  };

  std::shared_ptr<State> state_;

  ibmw::executor::ExecutorHandle timeout_executor_;
};

}  // namespace ibmw::runtime::core::util
