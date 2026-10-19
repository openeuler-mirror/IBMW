// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "cpp/util/json_type_support.h"
#include "type_support/type_support_pkg_main.h"
namespace {
struct Message {
  static constexpr const char* kTypeName = "PublicationFixture";
  int value = 0;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE(Message, value)
};
}
extern "C" const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  static const ibmw_type_support_base_t* entries[] = {ibmw::json::GetJsonMessageTypeSupport<Message>()};
  static const ibmw_type_support_package_v1_t package{
    .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_type_support_package_v1_t), .entry_count = 1, .entries = entries};
  return &package;
}
