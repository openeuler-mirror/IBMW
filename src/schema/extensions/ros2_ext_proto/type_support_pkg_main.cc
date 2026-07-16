// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_support/type_support_pkg_main.h"

#include "ros2_types/util/ros2_type_support_factory.h"

#include "ros2_ext_proto/msg/ros_msg_wrapper.hpp"

static const ibmw_type_support_base_t* type_support_array[]{
    ibmw::GetRos2MessageTypeSupport<ros2_ext_proto::msg::RosMsgWrapper>()};

extern "C" {

size_t IbmwDynlibGetTypeSupportArrayLength() {
  return sizeof(type_support_array) / sizeof(type_support_array[0]);
}

const ibmw_type_support_base_t** IbmwDynlibGetTypeSupportArray() {
  return type_support_array;
}
}
