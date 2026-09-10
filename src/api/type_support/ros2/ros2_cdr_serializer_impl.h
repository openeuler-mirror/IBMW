// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"
#include "ros2_cdr_serializer.h"

// ROS2 message type headers (only message definitions, not TypeSupport)
//
// Enable std_msgs/String when the public ROS 2 message header is visible to
// this target. The old top-level allocator<void> probe can run before ROS 2
// include directories are available and incorrectly disable Humble String
// serialization even though the target itself can compile the header.
#if __has_include(<std_msgs/msg/string.hpp>)
#include <std_msgs/msg/string.hpp>
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_STRING_HEADER 1
#else
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_STRING_HEADER 0
#endif

#if __has_include(<std_msgs/msg/int32.hpp>)
#include <std_msgs/msg/int32.hpp>
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_INT32 1
#else
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_INT32 0
#endif

#if __has_include(<std_msgs/msg/int64.hpp>)
#include <std_msgs/msg/int64.hpp>
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_INT64 1
#else
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_INT64 0
#endif

#if __has_include(<std_msgs/msg/float32.hpp>)
#include <std_msgs/msg/float32.hpp>
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_FLOAT32 1
#else
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_FLOAT32 0
#endif

#if __has_include(<std_msgs/msg/float64.hpp>)
#include <std_msgs/msg/float64.hpp>
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_FLOAT64 1
#else
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_FLOAT64 0
#endif

#if __has_include(<std_msgs/msg/bool.hpp>)
#include <std_msgs/msg/bool.hpp>
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_BOOL 1
#else
#define IBMW_ROS2_TYPES_HAS_STD_MSGS_BOOL 0
#endif

#if __has_include(<geometry_msgs/msg/vector3.hpp>)
#include <geometry_msgs/msg/vector3.hpp>
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_VECTOR3 1
#else
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_VECTOR3 0
#endif

#if __has_include(<geometry_msgs/msg/point.hpp>)
#include <geometry_msgs/msg/point.hpp>
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_POINT 1
#else
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_POINT 0
#endif

#if __has_include(<geometry_msgs/msg/quaternion.hpp>)
#include <geometry_msgs/msg/quaternion.hpp>
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_QUATERNION 1
#else
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_QUATERNION 0
#endif

#if __has_include(<geometry_msgs/msg/pose.hpp>)
#include <geometry_msgs/msg/pose.hpp>
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_POSE 1
#else
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_POSE 0
#endif

#if __has_include(<geometry_msgs/msg/twist.hpp>)
#include <geometry_msgs/msg/twist.hpp>
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_TWIST 1
#else
#define IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_TWIST 0
#endif

namespace ibmw::ros2 {

// ============================================================================
// std_msgs::msg::String
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_STD_MSGS_STRING_HEADER
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const std_msgs::msg::String& msg) {
  return 4 + 4 + msg.data.size() + 1;  // encapsulation + string length + content + null
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const std_msgs::msg::String& msg) {
  cdr << msg.data;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, std_msgs::msg::String& msg) {
  cdr >> msg.data;
}
#endif

// ============================================================================
// std_msgs::msg::Int32
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_STD_MSGS_INT32
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const std_msgs::msg::Int32& /*msg*/) {
  return 4 + 4;  // encapsulation + int32
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const std_msgs::msg::Int32& msg) {
  cdr << msg.data;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, std_msgs::msg::Int32& msg) {
  cdr >> msg.data;
}
#endif

// ============================================================================
// std_msgs::msg::Int64
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_STD_MSGS_INT64
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const std_msgs::msg::Int64& /*msg*/) {
  return 4 + 8;  // encapsulation + int64
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const std_msgs::msg::Int64& msg) {
  cdr << msg.data;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, std_msgs::msg::Int64& msg) {
  cdr >> msg.data;
}
#endif

// ============================================================================
// std_msgs::msg::Float32
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_STD_MSGS_FLOAT32
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const std_msgs::msg::Float32& /*msg*/) {
  return 4 + 4;  // encapsulation + float
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const std_msgs::msg::Float32& msg) {
  cdr << msg.data;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, std_msgs::msg::Float32& msg) {
  cdr >> msg.data;
}
#endif

// ============================================================================
// std_msgs::msg::Float64
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_STD_MSGS_FLOAT64
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const std_msgs::msg::Float64& /*msg*/) {
  return 4 + 8;  // encapsulation + double
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const std_msgs::msg::Float64& msg) {
  cdr << msg.data;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, std_msgs::msg::Float64& msg) {
  cdr >> msg.data;
}
#endif

// ============================================================================
// std_msgs::msg::Bool
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_STD_MSGS_BOOL
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const std_msgs::msg::Bool& /*msg*/) {
  return 4 + 1;  // encapsulation + bool
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const std_msgs::msg::Bool& msg) {
  cdr << msg.data;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, std_msgs::msg::Bool& msg) {
  cdr >> msg.data;
}
#endif

// ============================================================================
// geometry_msgs::msg::Vector3
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_VECTOR3
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const geometry_msgs::msg::Vector3& /*msg*/) {
  return 4 + 8 * 3;  // encapsulation + 3 doubles
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const geometry_msgs::msg::Vector3& msg) {
  cdr << msg.x << msg.y << msg.z;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, geometry_msgs::msg::Vector3& msg) {
  cdr >> msg.x >> msg.y >> msg.z;
}
#endif

// ============================================================================
// geometry_msgs::msg::Point
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_POINT
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const geometry_msgs::msg::Point& /*msg*/) {
  return 4 + 8 * 3;  // encapsulation + 3 doubles
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const geometry_msgs::msg::Point& msg) {
  cdr << msg.x << msg.y << msg.z;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, geometry_msgs::msg::Point& msg) {
  cdr >> msg.x >> msg.y >> msg.z;
}
#endif

// ============================================================================
// geometry_msgs::msg::Quaternion
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_QUATERNION
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const geometry_msgs::msg::Quaternion& /*msg*/) {
  return 4 + 8 * 4;  // encapsulation + 4 doubles
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const geometry_msgs::msg::Quaternion& msg) {
  cdr << msg.x << msg.y << msg.z << msg.w;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, geometry_msgs::msg::Quaternion& msg) {
  cdr >> msg.x >> msg.y >> msg.z >> msg.w;
}
#endif

// ============================================================================
// geometry_msgs::msg::Pose
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_POSE
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const geometry_msgs::msg::Pose& /*msg*/) {
  return 4 + 8 * 7;  // encapsulation + Point(3) + Quaternion(4)
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const geometry_msgs::msg::Pose& msg) {
  cdr << msg.position.x << msg.position.y << msg.position.z;
  cdr << msg.orientation.x << msg.orientation.y << msg.orientation.z << msg.orientation.w;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, geometry_msgs::msg::Pose& msg) {
  cdr >> msg.position.x >> msg.position.y >> msg.position.z;
  cdr >> msg.orientation.x >> msg.orientation.y >> msg.orientation.z >> msg.orientation.w;
}
#endif

// ============================================================================
// geometry_msgs::msg::Twist
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_GEOMETRY_MSGS_TWIST
template <>
inline uint32_t ROS2CdrSerializer::EstimateSize(const geometry_msgs::msg::Twist& /*msg*/) {
  return 4 + 8 * 6;  // encapsulation + 2 Vector3 (3 doubles each)
}

template <>
inline void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr, const geometry_msgs::msg::Twist& msg) {
  cdr << msg.linear.x << msg.linear.y << msg.linear.z;
  cdr << msg.angular.x << msg.angular.y << msg.angular.z;
}

template <>
inline void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr, geometry_msgs::msg::Twist& msg) {
  cdr >> msg.linear.x >> msg.linear.y >> msg.linear.z;
  cdr >> msg.angular.x >> msg.angular.y >> msg.angular.z;
}
#endif

}  // namespace ibmw::ros2
