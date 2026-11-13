// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Checks shared by the two TypeSupport package loaders (CORE-042):
// ibmw::runtime::core::util::TypeSupportPkgLoader and
// ibmw::extension::TypeSupportPackageLoader.

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "base/exception.h"
#include "cpp/util/schema_support.h"
#include "type_support/type_support_pkg_main.h"

namespace ibmw::extension::type_support_package {

using GetPackageV1Func = const ibmw_type_support_package_v1_t* (*)();

inline constexpr const char* kGetPackageV1FuncName = "IbmwDynlibGetTypeSupportPackageV1";

inline bool ValidString(ibmw_string_view_t value) noexcept {
  return value.str != nullptr && value.len != 0;
}

// Not noexcept: type_name() and the list callbacks are package code.
inline bool ValidTypeSupport(const ibmw_type_support_base_t* type_support) {
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
  const auto* serialization_types =
      type_support->serialization_types_supported_list(type_support->ctx);
  if (count == 0 || serialization_types == nullptr) {
    return false;
  }
  for (size_t index = 0; index < count; ++index) {
    if (!ValidString(serialization_types[index])) {
      return false;
    }
  }
  return true;
}

inline bool ValidSchema(const ibmw_schema_support_base_t* schema) noexcept {
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
                     std::string_view(schema->schema_data.str,
                                      schema->schema_data.len));
}

struct ValidatedPackage {
  std::span<const ibmw_type_support_base_t*> types;
  std::span<const ibmw_schema_support_base_t* const> schemas;
};

// Throws (MW_ASSERT) when the descriptor or any entry is invalid.
inline ValidatedPackage Validate(const ibmw_type_support_package_v1_t* package,
                                 std::string_view path) {
  MW_ASSERT(
      package == nullptr || package->abi_version != IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1,
      "Library {} exports a TypeSupport package of an older ABI (version 1); rebuild it against this "
      "IBMW (package ABI version 2).",
      path);
  MW_ASSERT(
      package != nullptr &&
          package->abi_version == IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V2 &&
          package->struct_size >= IBMW_TYPE_SUPPORT_PACKAGE_V1_REQUIRED_SIZE &&
          (package->entry_count == 0 || package->entries != nullptr),
      "Library {} exports an invalid TypeSupport package V1 descriptor.",
      path);

  ValidatedPackage result{.types = {package->entries, package->entry_count}, .schemas = {}};
  for (size_t index = 0; index < result.types.size(); ++index) {
    MW_ASSERT(
        ValidTypeSupport(result.types[index]),
        "Type support package {} exports invalid TypeSupport entry {}.",
        path, index);
  }

  if (package->schema_support != nullptr) {
    MW_ASSERT(
        package->schema_support->abi_version ==
                IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1 &&
            package->schema_support->struct_size >=
                IBMW_SCHEMA_SUPPORT_PACKAGE_V1_REQUIRED_SIZE &&
            (package->schema_support->entry_count == 0 ||
             package->schema_support->entries != nullptr),
        "Type support package {} exports an invalid SchemaSupport V1 capability.",
        path);
    result.schemas = {package->schema_support->entries,
                      package->schema_support->entry_count};
    for (size_t index = 0; index < result.schemas.size(); ++index) {
      MW_ASSERT(
          ValidSchema(result.schemas[index]),
          "Type support package {} exports invalid SchemaSupport V1 entry {}.",
          path, index);
    }
  }
  return result;
}

}  // namespace ibmw::extension::type_support_package
