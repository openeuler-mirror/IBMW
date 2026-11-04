// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "extension_api/type_support_package_loader.h"

#include <filesystem>

#include <dlfcn.h>

#include "base/exception.h"
#include "base/ibmw_config.h"
#include "engine/dynamic_lib.h"
#include "extension_api/type_support_package_validation.h"

namespace ibmw::extension {
namespace package_check = type_support_package;

namespace {

// Same flags as the runtime's DynamicLib (CORE-042).
#if defined(IBMW_ENABLE_DLOPEN_DEEPBIND) && defined(RTLD_DEEPBIND)
constexpr int kDlopenFlags = RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND;
#else
constexpr int kDlopenFlags = RTLD_NOW | RTLD_LOCAL;
#endif

struct DlCloser {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr) {
      ::dlclose(handle);
    }
  }
};

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
  // CORE-017: same trust rule as DynamicLib. A refused library is an error,
  // not "not a TypeSupport package".
  std::string dlopen_name;
  const auto untrusted = ibmw::common::util::CheckLibraryTrust(std::string(path), dlopen_name);
  MW_ASSERT(untrusted.empty(), "{}", untrusted);
  std::unique_ptr<void, DlCloser> candidate(::dlopen(dlopen_name.c_str(), kDlopenFlags));
  if (!candidate) {
    return false;
  }
  auto* symbol = ::dlsym(candidate.get(), package_check::kGetPackageV1FuncName);
  if (symbol == nullptr) {
    return false;
  }
  const auto validated =
      package_check::Validate(reinterpret_cast<package_check::GetPackageV1Func>(symbol)(), path);

  Unload();
  handle_ = candidate.release();
  type_supports_ = validated.types;
  schema_supports_ = validated.schemas;
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
