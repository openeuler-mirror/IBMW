// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <unordered_set>

#include "engine/service/service_driver.h"
#include "engine/rpc_client_tool.h"
#include "base/log_util.h"

namespace ibmw::runtime::core::service {

class InProcessServiceDriver : public ServiceDriver {
 public:
  struct Options {
    std::string timeout_executor;
  };

  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

 public:
  InProcessServiceDriver()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~InProcessServiceDriver() override = default;

  std::string_view Name() const noexcept override { return "local"; }

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  const Options& GetOptions() const { return options_; }
  State GetState() const { return state_.load(); }

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  void SetServiceCatalog(const ServiceCatalog* service_catalog_ptr) noexcept override {
    service_catalog_ptr_ = service_catalog_ptr;
  }

  bool RegisterServiceFunc(
      const ServiceFuncWrapper& service_func_wrapper) noexcept override;
  bool RegisterClientFunc(
      const ClientFuncWrapper& client_func_wrapper) noexcept override;
  void Call(
      const std::shared_ptr<ServiceMessageFrame>& client_call_wrapper_ptr) noexcept override;

  void RegisterGetExecutorFunc(
      const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func);

 private:
  Options options_;
  std::atomic<State> state_ = State::kPreInit;
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  const ServiceCatalog* service_catalog_ptr_ = nullptr;

  std::function<executor::ExecutorHandle(std::string_view)> get_executor_func_;

  std::atomic_uint32_t req_id_ = 0;

  // What the server-side result callbacks need. Shared by the driver and every
  // pending callback (instead of capturing `this`), so a server that answers
  // after Shutdown() or after the driver is destroyed touches no freed memory.
  struct CallbackContext {
    std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr;
    util::RpcClientTool<std::shared_ptr<ServiceMessageFrame>> client_tool;

    const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr; }

    void OnDirectResult(uint32_t req_id, ibmw::rpc::Status status);
    void OnSerializedResult(uint32_t cur_req_id,
                            const std::weak_ptr<ServiceMessageFrame>& service_invoke_weak,
                            const std::string& serialization_type,
                            ibmw::rpc::Status status);
  };
  std::shared_ptr<CallbackContext> cb_ctx_ptr_;

  using ServiceFuncIndexMap =
      std::unordered_map<
          std::string_view,  // func_name
          std::unordered_map<
              std::string_view,  // lib_path
              std::unordered_set<
                  std::string_view>>>;  // module_name
  ServiceFuncIndexMap service_func_register_index_;
};

}  // namespace ibmw::runtime::core::service
