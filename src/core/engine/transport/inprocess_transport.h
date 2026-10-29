// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

#include "cpp/executor/executor.h"
#include "engine/transport/transport_driver.h"
#include "base/log_util.h"

namespace ibmw::runtime::core::transport {

class InProcessTransport : public TransportDriver {
 public:
  struct Options {
    bool subscriber_use_inline_executor = true;
    std::string subscriber_executor;
  };

  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 public:
  InProcessTransport()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~InProcessTransport() override = default;

  std::string_view Name() const noexcept override { return "local"; }
 IBMW_DECLARE_DRIVER_SLOT("local")

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  const Options& GetOptions() const { return options_; }
  State GetState() const { return state_.load(); }

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  void SetTransportCatalog(const TransportCatalog* transport_catalog_ptr) noexcept override {
    transport_catalog_ptr_ = transport_catalog_ptr;
  }

  bool RegisterSendType(
      const SendTypeWrapper& send_type_wrapper) noexcept override;
  bool OnMessage(const MessageHandlerWrapper& message_handler) noexcept override;
  void Send(MessageFrame& msg_wrapper) noexcept override;

  void RegisterGetExecutorFunc(
      const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func);

 private:
  Options options_;
  std::atomic<State> state_ = State::kPreInit;
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  const TransportCatalog* transport_catalog_ptr_ = nullptr;

  std::function<executor::ExecutorHandle(std::string_view)> get_executor_func_;
  executor::ExecutorHandle subscribe_executor_ref_;

  using SubscribeIndexMap =
      std::unordered_map<
          std::string,  // msg_type
          std::unordered_map<
              std::string,  // topic
              std::unordered_map<
                  std::string,  // lib_path
                  std::unordered_set<
                      std::string>>>>;  // module_name
  SubscribeIndexMap subscribe_index_map_;

  // Counts Send() calls and queued async deliveries while a subscriber
  // executor is configured. Handlers are owned by the catalog, which is
  // released right after driver shutdown, so Shutdown() drains these first
  // (COMM-006). Unused (zero cost) with the default inline executor.
  class InflightToken {
   public:
    InflightToken(std::atomic<uint64_t>& counter, std::memory_order order) noexcept
        : counter_(&counter) { counter_->fetch_add(1, order); }
    InflightToken(InflightToken&& other) noexcept
        : counter_(std::exchange(other.counter_, nullptr)) {}
    InflightToken& operator=(InflightToken&&) = delete;
    ~InflightToken() {
      if (counter_) counter_->fetch_sub(1, std::memory_order_release);
    }

   private:
    std::atomic<uint64_t>* counter_;
  };
  void DrainInflight();
  void ReportIncompatibleCopy(const TransportInfo& pub_info, std::string_view sub_pkg_path) noexcept;

  alignas(64) std::atomic<uint64_t> inflight_{0};
  std::mutex incompatible_copy_mutex_;
  std::unordered_set<std::string> incompatible_copy_reported_;
};

}  // namespace ibmw::runtime::core::transport
