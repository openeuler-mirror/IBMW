// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "extension_api/type_support_package_loader.h"

#include <filesystem>

#include <dlfcn.h>

#include "base/exception.h"
#include "cpp/util/schema_support.h"
#include "type_support/type_support_pkg_main.h"

namespace ibmw::extension {
using DynlibGetTypeSupportPackageV1Func =
    const ibmw_type_support_package_v1_t* (*)();
constexpr const char* kDynlibGetTypeSupportPackageV1FuncName =
    "IbmwDynlibGetTypeSupportPackageV1";
namespace {

struct DlCloser {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr) {
      ::dlclose(handle);
    }
  }
};

bool ValidString(const ibmw_string_view_t& value) noexcept {
  return value.str != nullptr && value.len != 0;
}

bool ValidTypeSupport(const ibmw_type_support_base_t* type_support) noexcept {
  if (type_support == nullptr || type_support->type_name == nullptr ||
      type_support->create == nullptr || type_support->destroy == nullptr ||
      type_support->copy == nullptr || type_support->move == nullptr ||
      type_support->serialize == nullptr ||
      type_support->deserialize == nullptr ||
      type_support->serialization_types_supported_num == nullptr ||
      type_support->serialization_types_supported_list == nullptr ||
      type_support->custom_type_support_ptr == nullptr ||
      !ValidString(type_support->type_name(type_support->ctx))) {
    return false;
  }
  const size_t count =
      type_support->serialization_types_supported_num(type_support->ctx);
  const auto* formats =
      type_support->serialization_types_supported_list(type_support->ctx);
  if (count == 0 || formats == nullptr) {
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    if (!ValidString(formats[index])) {
      return false;
    }
  }
  return true;
}

bool ValidSchema(const ibmw_schema_support_base_t* schema) noexcept {
  if (schema == nullptr ||
      schema->abi_version != IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1 ||
      schema->struct_size < IBMW_SCHEMA_SUPPORT_V1_REQUIRED_SIZE ||
      !ValidString(schema->type_name) ||
      !ValidString(schema->serialization_type) ||
      !ValidString(schema->schema_name) ||
      !ValidString(schema->schema_encoding) ||
      !ValidString(schema->schema_data) || !ValidString(schema->schema_hash)) {
    return false;
  }
  const std::string_view hash(schema->schema_hash.str, schema->schema_hash.len);
  if (!hash.starts_with("fnv1a64:") || hash.size() != 24) {
    return false;
  }
  for (const char digit : hash.substr(8)) {
    if (!((digit >= '0' && digit <= '9') ||
          (digit >= 'a' && digit <= 'f'))) {
      return false;
    }
  }
  return hash == ibmw::util::ComputeSchemaHash(
                     std::string_view(
                         schema->schema_data.str, schema->schema_data.len));
}

}  // namespace

TypeSupportPackageLoader::~TypeSupportPackageLoader() {
  // TypeSupport DSOs can contain process-lifetime static registrations. Keep
  // successful packages mapped, matching the runtime's loader contract.
  handle_ = nullptr;
}

void TypeSupportPackageLoader::Load(std::string_view path) {
  MW_ASSERT(TryLoad(path), "Library '{}' is not a TypeSupport package.", path);
}

bool TypeSupportPackageLoader::TryLoad(std::string_view path) {
  std::unique_ptr<void, DlCloser> candidate(
      ::dlopen(std::string(path).c_str(), RTLD_NOW | RTLD_LOCAL));
  if (!candidate) {
    return false;
  }
  auto* symbol =
      ::dlsym(candidate.get(), kDynlibGetTypeSupportPackageV1FuncName);
  if (symbol == nullptr) {
    return false;
  }
  const auto* package = reinterpret_cast<DynlibGetTypeSupportPackageV1Func>(
      symbol)();
  MW_ASSERT(
      package != nullptr &&
          package->abi_version == IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1 &&
          package->struct_size >= IBMW_TYPE_SUPPORT_PACKAGE_V1_REQUIRED_SIZE &&
          (package->entry_count == 0 || package->entries != nullptr),
      "Library {} exports an invalid TypeSupport package V1 descriptor.",
      path);

  const std::span<const ibmw_type_support_base_t*> types(
      package->entries, package->entry_count);
  for (size_t index = 0; index < types.size(); ++index) {
    MW_ASSERT(
        ValidTypeSupport(types[index]),
        "TypeSupport package {} exports invalid entry {}.", path, index);
  }

  std::span<const ibmw_schema_support_base_t* const> schemas;
  if (package->schema_support != nullptr) {
    MW_ASSERT(
        package->schema_support->abi_version ==
                IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1 &&
            package->schema_support->struct_size >=
                IBMW_SCHEMA_SUPPORT_PACKAGE_V1_REQUIRED_SIZE &&
            (package->schema_support->entry_count == 0 ||
             package->schema_support->entries != nullptr),
        "TypeSupport package {} exports an invalid SchemaSupport capability.",
        path);
    schemas = std::span<const ibmw_schema_support_base_t* const>(
        package->schema_support->entries,
        package->schema_support->entry_count);
    for (size_t index = 0; index < schemas.size(); ++index) {
      MW_ASSERT(
          ValidSchema(schemas[index]),
          "TypeSupport package {} exports invalid schema entry {}.",
          path, index);
    }
  }

  Unload();
  handle_ = candidate.release();
  type_supports_ = types;
  schema_supports_ = schemas;
  Dl_info library_info{};
  const auto loaded_path = ::dladdr(symbol, &library_info) && library_info.dli_fname
                               ? std::string(library_info.dli_fname) : std::string(path);
  try {
    resolved_path_ = std::filesystem::canonical(loaded_path).string();
  } catch (...) {
    resolved_path_ = std::filesystem::absolute(loaded_path).string();
  }
  return true;
}

void TypeSupportPackageLoader::Unload() {
  type_supports_ = {};
  schema_supports_ = {};
  resolved_path_.clear();
  if (handle_ != nullptr) {
    ::dlclose(handle_);
    handle_ = nullptr;
  }
}

}  // namespace ibmw::extension
