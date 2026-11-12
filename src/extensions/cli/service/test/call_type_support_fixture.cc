// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "call_types.h"
#include "type_support/type_support_pkg_main.h"
extern "C" const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  using namespace ibmw::cli::service::test;
  static const ibmw_type_support_base_t* entries[] = {
      ibmw::json::GetJsonMessageTypeSupport<Request>(), ibmw::json::GetJsonMessageTypeSupport<Response>()};
  static const ibmw_type_support_package_v1_t package{
      .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1,
      .struct_size = sizeof(ibmw_type_support_package_v1_t), .entry_count = 2, .entries = entries};
  return &package;
}
