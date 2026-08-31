// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/type_support_registry.h"

#include "base/exception.h"

namespace ibmw::runtime::core::runtime_graph {

void TypeSupportRegistry::Load(std::string_view package_path) {
  MW_ASSERT(TryLoad(package_path), "Library '{}' is not a TypeSupport package.", package_path);
}

bool TypeSupportRegistry::TryLoad(std::string_view package_path) {
  std::lock_guard load_lock(load_mutex_);
  std::string request_key(package_path);
  // Bare library names retain dlopen's search semantics, even if a file with
  // that name exists in the current directory.
  if (package_path.find('/') != std::string_view::npos) {
    std::error_code error;
    const auto canonical = std::filesystem::canonical(package_path, error);
    if (!error) request_key = canonical.string();
  }
  if (loaded_package_paths_.contains(request_key)) return true;
  auto loader = std::make_unique<ibmw::extension::TypeSupportPackageLoader>();
  if (!loader->TryLoad(package_path)) {
    return false;
  }
  const auto resolved_path = loader->ResolvedPath();
  if (loaded_package_paths_.contains(resolved_path)) {
    loader->Unload();
    loaded_package_paths_.emplace(request_key);
    return true;
  }
  {
    std::lock_guard lock(mutex_);
    RegisterLoaded(resolved_path, std::move(loader));
  }
  loaded_package_paths_.emplace(resolved_path);
  loaded_package_paths_.emplace(request_key);
  return true;
}

void TypeSupportRegistry::RegisterLoaded(
    std::string package_path,
    std::unique_ptr<ibmw::extension::TypeSupportPackageLoader> loader) {
  for (const auto* item : loader->TypeSupports()) {
    ibmw::util::TypeSupportRef type_support(item);
    const std::string type_name(type_support.TypeName());
    TypeSupportEntry entry{package_path, type_support};
    MW_ASSERT(
        type_support_by_package_name_
            .emplace(PackageTypeKey(package_path, type_name), entry)
            .second,
        "Duplicate msg type '{}' in TypeSupport package {}.", type_name,
        package_path);
    const auto [fallback, inserted] =
        type_support_by_name_.emplace(type_name, entry);
    if (!inserted) {
      // Package-qualified lookup remains available; the first unqualified
      // registration is kept deterministic.
      static_cast<void>(fallback);
    }
  }
  for (const auto* item : loader->SchemaSupports()) {
    ibmw::util::SchemaSupportRef schema(item);
    RegisterSchema(schema.ToDescriptor());
  }
  loaders_.emplace_back(std::move(loader));
}

void TypeSupportRegistry::RegisterSchema(
    ibmw::util::SchemaDescriptor descriptor) {
  MW_ASSERT(!descriptor.type_name.empty(), "Schema type name cannot be empty.");
  MW_ASSERT(
      !descriptor.serialization_type.empty(),
      "Schema serialization type cannot be empty for '{}'.",
      descriptor.type_name);
  MW_ASSERT(
      !descriptor.schema_name.empty(), "Schema name cannot be empty for '{}'.",
      descriptor.type_name);
  MW_ASSERT(
      !descriptor.schema_encoding.empty(),
      "Schema encoding cannot be empty for '{}'.", descriptor.type_name);
  MW_ASSERT(
      !descriptor.schema_data.empty(), "Schema data cannot be empty for '{}'.",
      descriptor.type_name);
  MW_ASSERT(
      !descriptor.schema_hash.empty(), "Schema hash cannot be empty for '{}'.",
      descriptor.type_name);
  MW_ASSERT(
      descriptor.schema_hash ==
          ibmw::util::ComputeSchemaHash(descriptor.schema_data),
      "Schema hash mismatch for '{}'.", descriptor.type_name);

  const auto key = SchemaKey(
      descriptor.type_name, descriptor.serialization_type);
  const auto [existing, inserted] = schemas_.emplace(key, descriptor);
  if (!inserted) {
    const auto& old = existing->second;
    MW_ASSERT(
        old.schema_hash == descriptor.schema_hash &&
            old.schema_name == descriptor.schema_name &&
            old.schema_encoding == descriptor.schema_encoding &&
            old.schema_data == descriptor.schema_data,
        "Conflicting Schema Support for type '{}' and serialization '{}'.",
        descriptor.type_name, descriptor.serialization_type);
  }
}

ibmw::util::TypeSupportRef TypeSupportRegistry::Find(
    std::string_view msg_type,
    std::string_view package_path) const {
  std::lock_guard lock(mutex_);
  if (!package_path.empty()) {
    const auto exact = type_support_by_package_name_.find(
        PackageTypeKey(package_path, msg_type));
    if (exact != type_support_by_package_name_.end()) {
      return exact->second.type_support;
    }
  }
  const auto found = type_support_by_name_.find(std::string(msg_type));
  return found == type_support_by_name_.end()
             ? ibmw::util::TypeSupportRef{}
             : found->second.type_support;
}

std::string TypeSupportRegistry::PackagePath(
    std::string_view msg_type,
    std::string_view package_path) const {
  std::lock_guard lock(mutex_);
  if (!package_path.empty()) {
    const auto exact = type_support_by_package_name_.find(
        PackageTypeKey(package_path, msg_type));
    if (exact != type_support_by_package_name_.end()) {
      return exact->second.package_path;
    }
  }
  const auto found = type_support_by_name_.find(std::string(msg_type));
  return found == type_support_by_name_.end() ? std::string{}
                                               : found->second.package_path;
}

std::optional<ibmw::util::SchemaDescriptor> TypeSupportRegistry::FindSchema(
    std::string_view msg_type,
    std::string_view serialization_type) const {
  std::lock_guard lock(mutex_);
  const auto found = schemas_.find(SchemaKey(msg_type, serialization_type));
  return found == schemas_.end()
             ? std::nullopt
             : std::optional<ibmw::util::SchemaDescriptor>(found->second);
}

std::string TypeSupportRegistry::PackageTypeKey(
    std::string_view package_path,
    std::string_view msg_type) {
  return std::string(package_path) + '\0' + std::string(msg_type);
}

std::string TypeSupportRegistry::SchemaKey(
    std::string_view msg_type,
    std::string_view serialization_type) {
  return std::string(msg_type) + '\0' + std::string(serialization_type);
}

}  // namespace ibmw::runtime::core::runtime_graph
