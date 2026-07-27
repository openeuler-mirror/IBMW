// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdio>
#include <cstdlib>

#include "c/core_base.h"
#include "cpp/memory/allocator.h"
#include "cpp/transport/transport_handle.h"
#include "cpp/configurator/configurator.h"
#include "cpp/executor/executor_bus.h"
#include "cpp/logger/logger.h"
#include "cpp/parameter/parameter_handle.h"
#include "cpp/service/rpc_handle.h"

namespace ibmw {

struct ModuleDescriptor {
  std::string_view module_name;

  uint32_t major_version = 0;
  uint32_t minor_version = 0;
  uint32_t patch_version = 0;
  uint32_t build_version = 0;

  std::string_view author;
  std::string_view description;
};

/**
 * @brief Abstract of framework runtime, providing some functions for modules
 *
 */
class Runtime {
 public:
  Runtime() = default;
  explicit Runtime(const ibmw_core_base_t* base_ptr)
      : base_ptr_(base_ptr) {
    if (base_ptr_) {
      if (base_ptr_->abi_version != IBMW_CORE_ABI_VERSION) {
        std::fprintf(stderr,
                     "[IBMW FATAL] ABI version mismatch: runtime=%u, expected=%u. "
                     "Recompile all plugins against the current SDK.\n",
                     base_ptr_->abi_version, (unsigned)IBMW_CORE_ABI_VERSION);
        std::abort();
      }
      // A matching version is not sufficient if the provider supplied a
      // truncated or otherwise malformed current-version function table.
      // Validate the complete top-level table before invoking any accessor so
      // failure is closed and has no partial accessor side effects.
      if (base_ptr_->info == nullptr || base_ptr_->configurator == nullptr ||
          base_ptr_->executor_bus == nullptr || base_ptr_->logger == nullptr ||
          base_ptr_->memory_handle == nullptr ||
          base_ptr_->service_handle == nullptr ||
          base_ptr_->transport_handle == nullptr ||
          base_ptr_->parameter_handle == nullptr) {
        std::fprintf(stderr,
                     "[IBMW FATAL] Malformed current-version core ABI: "
                     "required accessor is null.\n");
        std::abort();
      }
      // Eagerly cache sub-handle pointers. These are stable after engine
      // initialization and never change for the lifetime of the module.
      // Caching eliminates repeated function-pointer indirection on every
      // GetXxx() call.
      cached_configurator_ = base_ptr_->configurator(base_ptr_->ctx);
      cached_executor_bus_ = base_ptr_->executor_bus(base_ptr_->ctx);
      cached_logger_ = base_ptr_->logger(base_ptr_->ctx);
      cached_memory_ = base_ptr_->memory_handle(base_ptr_->ctx);
      cached_service_ = base_ptr_->service_handle(base_ptr_->ctx);
      cached_transport_ = base_ptr_->transport_handle(base_ptr_->ctx);
      cached_parameter_ = base_ptr_->parameter_handle(base_ptr_->ctx);
      if (cached_configurator_ == nullptr || cached_executor_bus_ == nullptr ||
          cached_logger_ == nullptr || cached_memory_ == nullptr ||
          cached_service_ == nullptr || cached_transport_ == nullptr ||
          cached_parameter_ == nullptr) {
        std::fprintf(stderr,
                     "[IBMW FATAL] Malformed current-version core ABI: "
                     "required accessor returned null.\n");
        std::abort();
      }
    }
  }
  ~Runtime() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_core_base_t* NativeHandle() const { return base_ptr_; }

  ModuleDescriptor Info() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    MW_ASSERT(base_ptr_->info, "Core info accessor is null.");
    ibmw_module_info_t module_info = base_ptr_->info(base_ptr_->ctx);
    return ModuleDescriptor{
        .module_name = util::ToStdStringView(module_info.module_name),
        .major_version = module_info.major_version,
        .minor_version = module_info.minor_version,
        .patch_version = module_info.patch_version,
        .build_version = module_info.build_version,
        .author = util::ToStdStringView(module_info.author),
        .description = util::ToStdStringView(module_info.description)};
  }

  /**
   * @brief Get configurator handle
   *
   * @return ConfiguratorRef
   */
  configurator::ConfiguratorRef GetConfigurator() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return configurator::ConfiguratorRef(cached_configurator_);
  }

  /**
   * @brief Get allocator handle
   *
   * @return allocator::AllocatorRef
   */
  allocator::AllocatorRef GetMemory() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return allocator::AllocatorRef(cached_memory_);
  }

  /**
   * @brief Get executor bus handle
   *
   * @return ExecutorBusRef
   */
  executor::ExecutorBusRef GetExecutor() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return executor::ExecutorBusRef(cached_executor_bus_);
  }

  /**
   * @brief Get logger handle
   *
   * @return logger::LoggerRef
   */
  logger::LoggerRef GetLogger() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return logger::LoggerRef(cached_logger_);
  }

  /**
   * @brief Get service handle
   *
   * @return rpc::ServiceRef
   */
  rpc::ServiceRef GetService() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return rpc::ServiceRef(cached_service_);
  }

  /**
   * @brief Get transport handle
   *
   * @return transport::TransportRef
   */
  transport::TransportRef GetTransport() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return transport::TransportRef(cached_transport_);
  }

  /**
   * @brief Get parameter handle
   *
   * @return parameter::ParamsRef
   */
  parameter::ParamsRef GetParams() const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return parameter::ParamsRef(cached_parameter_);
  }

 private:
  const ibmw_core_base_t* base_ptr_ = nullptr;

  // Cached sub-handle pointers (populated once in constructor).
  const ibmw_configurator_base_t* cached_configurator_ = nullptr;
  const ibmw_executor_bus_base_t* cached_executor_bus_ = nullptr;
  const ibmw_logger_base_t* cached_logger_ = nullptr;
  const ibmw_allocator_base_t* cached_memory_ = nullptr;
  const ibmw_service_handle_base_t* cached_service_ = nullptr;
  const ibmw_transport_handle_base_t* cached_transport_ = nullptr;
  const ibmw_parameter_handle_base_t* cached_parameter_ = nullptr;
};

}  // namespace ibmw
