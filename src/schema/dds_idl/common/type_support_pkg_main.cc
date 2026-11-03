// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_support/type_support_pkg_main.h"

#include "type_support/dds/dds_type_support.h"

#include "ErrorCdrAux.ipp"
#include "ErrorSchema.hpp"
#include "HeaderCdrAux.ipp"
#include "HeaderSchema.hpp"

using Header = ibmw::protocols::common::Header;
using Error = ibmw::protocols::common::Error;

IBMW_DDS_TYPE_NAME(Header, "ibmw::protocols::common::Header")
IBMW_DDS_TYPE_NAME(Error, "ibmw::protocols::common::Error")

namespace {

const ibmw_type_support_base_t* type_support_array[]{
    ibmw::dds::GetDdsMessageTypeSupport<Header>(),
    ibmw::dds::GetDdsMessageTypeSupport<Error>()};

const ibmw_schema_support_base_t* schema_support_array[]{
    ibmw::dds::GetDdsMessageSchemaSupport<Header>(
        "ibmw::protocols::common::Header", ibmw::dds::schema::kHeaderIdl),
    ibmw::dds::GetDdsMessageSchemaSupport<Error>(
        "ibmw::protocols::common::Error", ibmw::dds::schema::kErrorIdl)};

const ibmw_schema_support_package_v1_t schema_support_package{
    .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_schema_support_package_v1_t),
    .entry_count = sizeof(schema_support_array) / sizeof(schema_support_array[0]),
    .entries = schema_support_array,
    .reserved = {}};

const ibmw_type_support_package_v1_t type_support_package{
    .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V2,
    .struct_size = sizeof(ibmw_type_support_package_v1_t),
    .entry_count = sizeof(type_support_array) / sizeof(type_support_array[0]),
    .entries = type_support_array,
    .schema_support = &schema_support_package,
    .reserved = {}};

}  // namespace

extern "C" {

const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  return &type_support_package;
}

}  // extern "C"
