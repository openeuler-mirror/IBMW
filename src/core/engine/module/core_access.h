// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/core_base.h"

namespace ibmw::runtime::core::module {

class CoreAccess {
 public:
  explicit CoreAccess(ibmw_module_info_t module_info)
      : module_info_(module_info), base_(GenBase(this)) {}
  ~CoreAccess() = default;

  CoreAccess(const CoreAccess&) = delete;
  CoreAccess& operator=(const CoreAccess&) = delete;

  void SetConfigurator(const ibmw_configurator_base_t* ptr) {
    configurator_ = ptr;
  }

  void SetExecutorBus(const ibmw_executor_bus_base_t* ptr) {
    executor_bus_ = ptr;
  }

  void SetLogger(const ibmw_logger_base_t* ptr) {
    logger_ = ptr;
  }

  void SetMemory(const ibmw_allocator_base_t* ptr) {
    memory_handle_ = ptr;
  }

  void SetServiceHandle(const ibmw_service_handle_base_t* ptr) {
    service_handle_ = ptr;
  }

  void SetTransportHandle(const ibmw_transport_handle_base_t* ptr) {
    transport_handle_ = ptr;
  }

  void SetParameterHandle(const ibmw_parameter_handle_base_t* ptr) {
    parameter_handle_ = ptr;
  }

  const ibmw_core_base_t* NativeHandle() const { return &base_; }

 private:
  static ibmw_core_base_t GenBase(void* ctx) {
    return ibmw_core_base_t{
        .abi_version = IBMW_CORE_ABI_VERSION,
        .struct_size = sizeof(ibmw_core_base_t),
        .info = [](void* ctx) -> ibmw_module_info_t {
          return static_cast<CoreAccess*>(ctx)->module_info_;
        },
        .configurator = [](void* ctx) -> const ibmw_configurator_base_t* {
          return static_cast<CoreAccess*>(ctx)->configurator_;
        },
        .executor_bus = [](void* ctx) -> const ibmw_executor_bus_base_t* {
          return static_cast<CoreAccess*>(ctx)->executor_bus_;
        },
        .logger = [](void* ctx) -> const ibmw_logger_base_t* {
          return static_cast<CoreAccess*>(ctx)->logger_;
        },
        .memory_handle = [](void* ctx) -> const ibmw_allocator_base_t* {
          return static_cast<CoreAccess*>(ctx)->memory_handle_;
        },
        .service_handle = [](void* ctx) -> const ibmw_service_handle_base_t* {
          return static_cast<CoreAccess*>(ctx)->service_handle_;
        },
        .transport_handle = [](void* ctx) -> const ibmw_transport_handle_base_t* {
          return static_cast<CoreAccess*>(ctx)->transport_handle_;
        },
        .parameter_handle = [](void* ctx) -> const ibmw_parameter_handle_base_t* {
          return static_cast<CoreAccess*>(ctx)->parameter_handle_;
        },
        .ctx = ctx};
  }

 private:
  const ibmw_module_info_t module_info_;

  const ibmw_configurator_base_t* configurator_ = nullptr;
  const ibmw_executor_bus_base_t* executor_bus_ = nullptr;
  const ibmw_logger_base_t* logger_ = nullptr;
  const ibmw_allocator_base_t* memory_handle_ = nullptr;
  const ibmw_service_handle_base_t* service_handle_ = nullptr;
  const ibmw_transport_handle_base_t* transport_handle_ = nullptr;
  const ibmw_parameter_handle_base_t* parameter_handle_ = nullptr;

  const ibmw_core_base_t base_;
};

}  // namespace ibmw::runtime::core::module
