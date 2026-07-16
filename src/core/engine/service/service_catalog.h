// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <unordered_map>

#include "engine/service/service_message_frame.h"
#include "base/log_util.h"

namespace ibmw::runtime::core::service {

using ServiceFunc = std::function<void(const std::shared_ptr<ServiceMessageFrame>&)>;

struct ServiceFuncWrapper {
  ServiceInfo info;
  ServiceFunc service_func;
};

struct ClientFuncWrapper {
  ServiceInfo info;
};

class ServiceCatalog {
 public:
  ServiceCatalog()
      : logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}
  ~ServiceCatalog() = default;

  ServiceCatalog(const ServiceCatalog&) = delete;
  ServiceCatalog& operator=(const ServiceCatalog&) = delete;

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) { logger_ptr_ = logger_ptr; }
  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  bool RegisterServiceFunc(
      std::unique_ptr<ServiceFuncWrapper>&& service_func_wrapper_ptr);

  bool RegisterClientFunc(
      std::unique_ptr<ClientFuncWrapper>&& client_func_wrapper_ptr);

  const ServiceFuncWrapper* GetServiceFuncWrapperPtr(
      std::string_view func_name, std::string_view pkg_path, std::string_view module_name) const;

  const ClientFuncWrapper* GetClientFuncWrapperPtr(
      std::string_view func_name, std::string_view pkg_path, std::string_view module_name) const;

  const auto& GetServiceFuncWrapperMap() const { return service_func_wrapper_map_; }
  const auto& GetClientFuncWrapperMap() const { return client_func_wrapper_map_; }

  const auto& GetServiceIndexMap() const { return service_index_map_; }
  const auto& GetClientIndexMap() const { return client_index_map_; }

 private:
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  struct Key {
    std::string_view func_name;
    std::string_view pkg_path;
    std::string_view module_name;

    bool operator==(const Key& rhs) const {
      return func_name == rhs.func_name &&
             pkg_path == rhs.pkg_path &&
             module_name == rhs.module_name;
    }

    struct Hash {
      std::size_t operator()(const Key& k) const {
        return (std::hash<std::string_view>()(k.func_name)) ^
               (std::hash<std::string_view>()(k.pkg_path)) ^
               (std::hash<std::string_view>()(k.module_name));
      }
    };
  };

  std::unordered_map<Key, std::unique_ptr<ServiceFuncWrapper>, Key::Hash>
      service_func_wrapper_map_;

  // Index table, func_name:wrapper
  std::unordered_map<std::string_view, std::vector<ServiceFuncWrapper*>>
      service_index_map_;

  std::unordered_map<Key, std::unique_ptr<ClientFuncWrapper>, Key::Hash>
      client_func_wrapper_map_;

  // Index table, func_name:wrapper
  std::unordered_map<std::string_view, std::vector<ClientFuncWrapper*>>
      client_index_map_;
};
}  // namespace ibmw::runtime::core::service
