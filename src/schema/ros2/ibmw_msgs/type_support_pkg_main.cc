// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_support/type_support_pkg_main.h"

#include "ros2_types/util/ros2_type_support_factory.h"

#include "ibmw_msgs/msg/joint_command.hpp"
#include "ibmw_msgs/msg/joint_command_array.hpp"
#include "ibmw_msgs/msg/joint_state.hpp"
#include "ibmw_msgs/msg/joint_state_array.hpp"
#include "ibmw_msgs/msg/message_header.hpp"
#include "ibmw_msgs/msg/touch_sensor_state.hpp"
#include "ibmw_msgs/msg/touch_sensor_state_array.hpp"

static const ibmw_type_support_base_t* type_support_array[]{
    ibmw::GetRos2MessageTypeSupport<ibmw_msgs::msg::MessageHeader>(),
    ibmw::GetRos2MessageTypeSupport<ibmw_msgs::msg::TouchSensorStateArray>(),
    ibmw::GetRos2MessageTypeSupport<ibmw_msgs::msg::TouchSensorState>(),
    ibmw::GetRos2MessageTypeSupport<ibmw_msgs::msg::JointStateArray>(),
    ibmw::GetRos2MessageTypeSupport<ibmw_msgs::msg::JointState>(),
    ibmw::GetRos2MessageTypeSupport<ibmw_msgs::msg::JointCommandArray>(),
    ibmw::GetRos2MessageTypeSupport<ibmw_msgs::msg::JointCommand>()};

extern "C" {

size_t IbmwDynlibGetTypeSupportArrayLength() {
  return sizeof(type_support_array) / sizeof(type_support_array[0]);
}

const ibmw_type_support_base_t** IbmwDynlibGetTypeSupportArray() {
  return type_support_array;
}
}
