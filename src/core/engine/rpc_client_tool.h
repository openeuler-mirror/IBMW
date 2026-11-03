// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>

#include "base/ibmw_config.h"
#include "cpp/executor/executor.h"

#if defined(IBMW_SELFDEV_CONCURRENT)
  #include "ibmw_selfdev/concurrent/concurrent_hash_map.h"
#else
  #include "tbb/concurrent_hash_map.h"
#endif

namespace ibmw::runtime::core::util {

template <typename MsgRecorder>
class RpcClientTool {
 public:
  using TimeoutHandle = std::function<void(MsgRecorder&&)>;

 public:
  RpcClientTool() : shutdown_flag_(std::make_shared<std::atomic_bool>(false)) {}
  ~RpcClientTool() { shutdown_flag_->store(true); }

  void RegisterTimeoutExecutor(ibmw::executor::ExecutorHandle timeout_executor) {
    if (!timeout_executor.SupportsTimedDispatch())
      throw std::runtime_error("Timeout executor do not support timer schedule.");

    timeout_executor_ = timeout_executor;
  }

  void RegisterTimeoutHandle(const TimeoutHandle& timeout_handle) {
    timeout_handle_ = timeout_handle;
  }

  bool Record(uint32_t req_id, std::chrono::nanoseconds timeout, MsgRecorder&& msg_recorder) {
#if defined(IBMW_SELFDEV_CONCURRENT)
    bool ret = client_msg_recorder_map_.Emplace(req_id, std::move(msg_recorder));
#else
    bool ret = client_msg_recorder_map_.emplace(req_id, std::move(msg_recorder));
#endif

    if (!ret) [[unlikely]]
      return false;

    if (timeout_executor_) {
      // Capture shutdown_flag by shared_ptr so the timeout lambda
      // remains safe even after RpcClientTool is destroyed.
      auto flag = shutdown_flag_;
      timeout_executor_.DispatchAfter(timeout, [this, req_id, flag]() {
        if (flag->load()) [[unlikely]]
          return;

        auto msg_recorder_op = GetRecord(req_id);
        if (msg_recorder_op) [[unlikely]]
          timeout_handle_(std::move(*msg_recorder_op));
      });
    }

    return true;
  }

  std::optional<MsgRecorder> GetRecord(uint32_t req_id) {
#if defined(IBMW_SELFDEV_CONCURRENT)
    // Find and erase under one shard lock: a timeout racing the response for
    // the same req_id gets the record exactly once.
    return client_msg_recorder_map_.Take(req_id);
#else
    typename ClientMsgRecorderMap::accessor ac;
    bool find_ret = client_msg_recorder_map_.find(ac, req_id);

    if (!find_ret) [[unlikely]]
      return {};

    auto result = std::make_optional<MsgRecorder>(std::move(ac->second));

    client_msg_recorder_map_.erase(ac);

    return result;
#endif
  }

 private:
  std::shared_ptr<std::atomic_bool> shutdown_flag_;

  ibmw::executor::ExecutorHandle timeout_executor_;
  TimeoutHandle timeout_handle_;

#if defined(IBMW_SELFDEV_CONCURRENT)
  using ClientMsgRecorderMap = ibmw::selfdev::concurrent::ConcurrentHashMap<uint32_t, MsgRecorder>;
  // Few requests are in flight per client and every client has its own map:
  // 64 shards keep the shard headers at ~8 KiB per client instead of the
  // default ~64 KiB.
  static constexpr size_t kRecorderShards = 64;
  ClientMsgRecorderMap client_msg_recorder_map_{kRecorderShards};
#else
  using ClientMsgRecorderMap = tbb::concurrent_hash_map<uint32_t, MsgRecorder>;
  ClientMsgRecorderMap client_msg_recorder_map_;
#endif
};

}  // namespace ibmw::runtime::core::util
