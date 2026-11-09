// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "messages.h"
#include "cpp/util/json_type_support.h"
#include "type_support/type_support_pkg_main.h"

extern "C" const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  using namespace ibmw::diag::fixture;
  static const ibmw_type_support_base_t* entries[] = {
      ibmw::json::GetJsonMessageTypeSupport<Message>(),
      ibmw::json::GetJsonMessageTypeSupport<SumRequest>(),
      ibmw::json::GetJsonMessageTypeSupport<SumResponse>()};
  // Runtime Tap requires the schema exported by a normal generated type package.
  static const ibmw_schema_support_base_t* schemas[] = {
      ibmw::json::GetJsonMessageSchemaSupport<Message>()};
  static const ibmw_schema_support_package_v1_t schema_package{
      .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
      .struct_size = sizeof(ibmw_schema_support_package_v1_t),
      .entry_count = 1, .entries = schemas};
  static const ibmw_type_support_package_v1_t package{
      .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1,
      .struct_size = sizeof(ibmw_type_support_package_v1_t),
      .entry_count = 3, .entries = entries, .schema_support = &schema_package};
  return &package;
}
