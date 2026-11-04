// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_support/type_support_pkg_main.h"

namespace {
template <size_t N> constexpr ibmw_string_view_t View(const char (&text)[N]) {
  return {.str = text, .len = N - 1};
}
const ibmw_schema_support_v1_t json_schema{
    .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_schema_support_v1_t),
    .type_name = View("test:AutoSchema"), .serialization_type = View("json"),
    .schema_name = View("AutoSchema"), .schema_encoding = View("jsonschema"),
    .schema_data = View("{}"), .schema_hash = View("fnv1a64:08f44b07b5901a25")};
const ibmw_schema_support_v1_t cdr_schema = [] {
  auto result = json_schema;
  result.serialization_type = View("cdr");
  return result;
}();
const ibmw_schema_support_v1_t* schemas[] = {&json_schema, &cdr_schema};
const ibmw_schema_support_package_v1_t schema_package{
    .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_schema_support_package_v1_t),
    .entry_count = 2, .entries = schemas};
}

// No serializers: auto capture must need only schema metadata.
extern "C" const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  static const ibmw_type_support_package_v1_t package{
      .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V2,
      .struct_size = sizeof(ibmw_type_support_package_v1_t),
      .entry_count = 0, .entries = nullptr, .schema_support = &schema_package};
  return &package;
}
