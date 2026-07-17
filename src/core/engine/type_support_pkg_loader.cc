// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/type_support_pkg_loader.h"

#include <cstdio>

#include "base/exception.h"

namespace ibmw::runtime::core::util {

using DynlibGetTypeSupportArrayLengthFunc = size_t (*)();
using DynlibGetTypeSupportArray = const ibmw_type_support_base_t** (*)();

static constexpr const char* kDynlibGetTypeSupportArrayLengthFuncName = "IbmwDynlibGetTypeSupportArrayLength";
static constexpr const char* kDynlibGetTypeSupportArrayFuncName = "IbmwDynlibGetTypeSupportArray";

void TypeSupportPkgLoader::LoadTypeSupportPkg(std::string_view path) {
  path_ = path;

  MW_ASSERT(dynamic_lib_.Load(path_),
              "Load dynamic lib failed, lib path {}, error info {}",
              path_, ibmw::common::util::DynamicLib::GetErr());

  auto* get_length_func = dynamic_lib_.GetSymbol(kDynlibGetTypeSupportArrayLengthFuncName);

  MW_ASSERT(get_length_func != nullptr,
              "Cannot find symbol '{}' in lib {}.",
              kDynlibGetTypeSupportArrayLengthFuncName, path_);

  auto* get_array_func = dynamic_lib_.GetSymbol(kDynlibGetTypeSupportArrayFuncName);

  MW_ASSERT(get_array_func != nullptr,
              "Cannot find symbol '{}' in lib {}.",
              kDynlibGetTypeSupportArrayFuncName, path_);

  size_t array_size = ((DynlibGetTypeSupportArrayLengthFunc)get_length_func)();
  const ibmw_type_support_base_t** array_ptr = ((DynlibGetTypeSupportArray)get_array_func)();

  MW_ASSERT(array_ptr != nullptr,
              "Cannot get type support array in lib {}.",
              path_);

  type_support_array_ = std::span{array_ptr, array_size};
}

void TypeSupportPkgLoader::UnLoadTypeSupportPkg() {
  if (!dynamic_lib_.IsLoaded()) return;

  dynamic_lib_.Unload();
}

}  // namespace ibmw::runtime::core::util
