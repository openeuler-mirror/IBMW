// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <span>

#include "c/util/schema_support_base.h"
#include "c/util/type_support_base.h"
#include "type_support/type_support_pkg_main.h"

#include "engine/dynamic_lib.h"

namespace ibmw::runtime::core::util {

class TypeSupportPkgLoader {
 public:
  TypeSupportPkgLoader() = default;
  ~TypeSupportPkgLoader() {
    // Intentionally do NOT call UnLoadTypeSupportPkg() here.
    // Type support .so files may contain static objects whose destructors
    // reference resources already freed. The OS reclaims on process exit.
    dynamic_lib_.LeakHandle();
  }

  TypeSupportPkgLoader(const TypeSupportPkgLoader&) = delete;
  TypeSupportPkgLoader& operator=(const TypeSupportPkgLoader&) = delete;

  void LoadTypeSupportPkg(std::string_view path);
  bool TryLoadTypeSupportPkg(std::string_view path);

  void UnLoadTypeSupportPkg();

  auto GetTypeSupportArray() const { return type_support_array_; }

  auto GetSchemaSupportArray() const { return schema_support_array_; }

  auto& GetDynamicLib() { return dynamic_lib_; }

 private:
  std::string path_;
  ibmw::common::util::DynamicLib dynamic_lib_;

  std::span<const ibmw_type_support_base_t*> type_support_array_;
  std::span<const ibmw_schema_support_base_t* const> schema_support_array_;
};

}  // namespace ibmw::runtime::core::util
