// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "c/util/schema_support_base.h"
#include "c/util/type_support_base.h"

namespace ibmw::extension {

class TypeSupportPackageLoader {
 public:
  TypeSupportPackageLoader() = default;
  ~TypeSupportPackageLoader();

  TypeSupportPackageLoader(const TypeSupportPackageLoader&) = delete;
  TypeSupportPackageLoader& operator=(const TypeSupportPackageLoader&) = delete;

  void Load(std::string_view path);
  // Returns false when path is not a TypeSupport package; throws when a library
  // given by path could be replaced by other users (CORE-017).
  bool TryLoad(std::string_view path);
  void Unload();

  std::span<const ibmw_type_support_base_t*> TypeSupports() const {
    return type_supports_;
  }
  std::span<const ibmw_schema_support_base_t* const> SchemaSupports() const {
    return schema_supports_;
  }
  const std::string& ResolvedPath() const { return resolved_path_; }

 private:
  void* handle_ = nullptr;
  std::string resolved_path_;
  std::span<const ibmw_type_support_base_t*> type_supports_;
  std::span<const ibmw_schema_support_base_t* const> schema_supports_;
};

}  // namespace ibmw::extension
