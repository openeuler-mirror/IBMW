// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <list>

#include "engine/service/service_catalog.h"

#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::service {

class ServiceDriver {
 public:
  ServiceDriver() = default;
  virtual ~ServiceDriver() = default;

  ServiceDriver(const ServiceDriver&) = delete;
  ServiceDriver& operator=(const ServiceDriver&) = delete;

  virtual std::string_view Name() const noexcept = 0;  // It should always return the same value

  // Implementations assign their effective options back to options_node.
  // YAML::Node shares storage with the caller's tree, so this fills in the
  // config that --dump_cfg writes out; it is not a dead store.
  virtual void Initialize(YAML::Node options_node) = 0;
  virtual void Start() = 0;
  virtual void Shutdown() = 0;

  virtual std::list<std::pair<std::string, std::string>> GenInitializationReport() const noexcept { return {}; }

  /**
   * @brief Set the Rpc Catalog to driver
   * @note
   * 1. This method will only be called once before 'Initialize'.
   *
   * @param service_catalog_ptr
   */
  virtual void SetServiceCatalog(const ServiceCatalog* service_catalog_ptr) noexcept {}

  /**
   * @brief Register service func
   * @note
   * 1. This method will only be called after 'Initialize' and before 'Start'.
   *
   * @param service_func_wrapper
   * @return Register result
   */
  virtual bool RegisterServiceFunc(
      const ServiceFuncWrapper& service_func_wrapper) noexcept = 0;

  /**
   * @brief Register client func
   * @note
   * 1. This method will only be called after 'Initialize' and before 'Start'.
   *
   * @param client_func_wrapper
   * @return Register result
   */
  virtual bool RegisterClientFunc(
      const ClientFuncWrapper& client_func_wrapper) noexcept = 0;

  /**
   * @brief Invoke a service call
   * @note
   * 1. This method will only be called after 'Start' and before 'Shutdown'.
   *
   * @param client_call_wrapper_ptr
   */
  virtual void Call(
      const std::shared_ptr<ServiceMessageFrame>& client_call_wrapper_ptr) noexcept = 0;
};

}  // namespace ibmw::runtime::core::service
