// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <memory>
#include "call_options.h"
#include "cpp/util/type_support.h"
#include "extension_api/type_support_package_loader.h"

namespace ibmw::cli::service {
// Internal optional adapter. It is not a public Package or TypeSupport ABI.
struct NativeCallTypes {
  virtual ~NativeCallTypes() = default;
  ibmw::util::TypeSupportRef request, response;
  const void* service = nullptr;
};
struct CallTypes {
  std::vector<std::unique_ptr<ibmw::extension::TypeSupportPackageLoader>> packages;
  std::shared_ptr<void> native_library;
  std::unique_ptr<NativeCallTypes> native_types;
  ibmw::util::TypeSupportRef request, response;
  const void* custom_service = nullptr;
  std::string function, serialization;
};
CallTypes LoadCallTypes(const CallOptions& options);
std::string ParseCallRequest(std::string_view text);
}  // namespace ibmw::cli::service
