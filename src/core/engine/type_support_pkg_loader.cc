// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/type_support_pkg_loader.h"

#include <string_view>

#include "base/exception.h"
#include "extension_api/type_support_package_validation.h"

namespace ibmw::runtime::core::util {

namespace package_check = ibmw::extension::type_support_package;

void TypeSupportPkgLoader::LoadTypeSupportPkg(std::string_view path) {
  MW_ASSERT(
      TryLoadTypeSupportPkg(path),
      "Library '{}' is not a TypeSupport package.", path);
}

bool TypeSupportPkgLoader::TryLoadTypeSupportPkg(std::string_view path) {
  std::string candidate_path(path);
  ibmw::common::util::DynamicLib candidate;
  if (!candidate.Load(candidate_path)) {
    return false;
  }

  auto* get_package_func = candidate.GetSymbol(package_check::kGetPackageV1FuncName);
  if (get_package_func == nullptr) {
    return false;
  }

  const auto validated = package_check::Validate(
      reinterpret_cast<package_check::GetPackageV1Func>(get_package_func)(), candidate_path);

  dynamic_lib_.Swap(candidate);
  path_ = std::move(candidate_path);
  type_support_array_ = validated.types;
  schema_support_array_ = validated.schemas;
  return true;
}

void TypeSupportPkgLoader::UnLoadTypeSupportPkg() {
  if (!dynamic_lib_.IsLoaded()) return;

  type_support_array_ = {};
  schema_support_array_ = {};
  dynamic_lib_.Unload();
  path_.clear();
}

}  // namespace ibmw::runtime::core::util
