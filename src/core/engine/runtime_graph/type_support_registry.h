// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "cpp/util/schema_support.h"
#include "cpp/util/type_support.h"
#include "extension_api/type_support_package_loader.h"

namespace ibmw::runtime::core::runtime_graph {

// Process-local registry shared by Runtime Graph and command-line clients.
// It owns loaded TypeSupport libraries for the lifetime of the registry and
// provides package-aware type lookup plus Schema lookup.
class TypeSupportRegistry final {
 public:
  TypeSupportRegistry() = default;
  ~TypeSupportRegistry() = default;

  TypeSupportRegistry(const TypeSupportRegistry&) = delete;
  TypeSupportRegistry& operator=(const TypeSupportRegistry&) = delete;

  void Load(std::string_view package_path);
  bool TryLoad(std::string_view package_path);

  ibmw::util::TypeSupportRef Find(
      std::string_view msg_type,
      std::string_view package_path = {}) const;
  std::string PackagePath(
      std::string_view msg_type,
      std::string_view package_path = {}) const;
  std::optional<ibmw::util::SchemaDescriptor> FindSchema(
      std::string_view msg_type,
      std::string_view serialization_type) const;

 private:
  struct TypeSupportEntry {
    std::string package_path;
    ibmw::util::TypeSupportRef type_support;
  };

  static std::string PackageTypeKey(
      std::string_view package_path,
      std::string_view msg_type);
  static std::string SchemaKey(
      std::string_view msg_type,
      std::string_view serialization_type);

  void RegisterLoaded(
      std::string package_path,
      std::unique_ptr<ibmw::extension::TypeSupportPackageLoader> loader);
  static void ValidateSchema(const ibmw::util::SchemaDescriptor& descriptor);
  static void RequireSameSchema(
      const ibmw::util::SchemaDescriptor& old,
      const ibmw::util::SchemaDescriptor& descriptor);

  mutable std::mutex mutex_;
  std::mutex load_mutex_;
  std::set<std::string> loaded_package_paths_;
  std::vector<std::unique_ptr<ibmw::extension::TypeSupportPackageLoader>> loaders_;
  std::unordered_map<std::string, TypeSupportEntry> type_support_by_name_;
  std::unordered_map<std::string, TypeSupportEntry>
      type_support_by_package_name_;
  std::unordered_map<std::string, ibmw::util::SchemaDescriptor> schemas_;
};

}  // namespace ibmw::runtime::core::runtime_graph
