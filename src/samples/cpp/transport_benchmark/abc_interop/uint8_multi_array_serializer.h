#pragma once

#include <cstdint>

#include <std_msgs/msg/u_int8_multi_array.hpp>

#include "ros2_types/util/ros2_cdr_serializer.h"

namespace ibmw::ros2 {

template <>
bool ROS2CdrSerializer::Serialize<std_msgs::msg::UInt8MultiArray>(
    const std_msgs::msg::UInt8MultiArray& msg,
    rtps_compat::SerializedPayload_t* payload);

template <>
uint32_t ROS2CdrSerializer::EstimateSize(
    const std_msgs::msg::UInt8MultiArray& msg);

template <>
void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr,
    const std_msgs::msg::UInt8MultiArray& msg);

template <>
void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr,
    std_msgs::msg::UInt8MultiArray& msg);

}  // namespace ibmw::ros2
