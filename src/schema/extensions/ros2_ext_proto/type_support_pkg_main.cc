// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_support/type_support_pkg_main.h"

#include "type_support/ros2/ros2_schema_support.h"
#include "type_support/ros2/ros2_type_support_factory.h"

#include "ros2_ext_proto/msg/ros_msg_wrapper.hpp"

static const ibmw_type_support_base_t* type_support_array[]{
    ibmw::GetRos2MessageTypeSupport<ros2_ext_proto::msg::RosMsgWrapper>()};

static const ibmw_schema_support_base_t* schema_support_array[]{
    ibmw::ros2::GetRos2MessageSchemaSupport<
        ros2_ext_proto::msg::RosMsgWrapper>()};

static const ibmw_schema_support_package_v1_t schema_support_package{
    .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_schema_support_package_v1_t),
    .entry_count = sizeof(schema_support_array) / sizeof(schema_support_array[0]),
    .entries = schema_support_array,
    .reserved = {}};

static const ibmw_type_support_package_v1_t type_support_package{
    .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_type_support_package_v1_t),
    .entry_count = sizeof(type_support_array) / sizeof(type_support_array[0]),
    .entries = type_support_array,
    .schema_support = &schema_support_package,
    .reserved = {}};

extern "C" {

const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  return &type_support_package;
}
}
