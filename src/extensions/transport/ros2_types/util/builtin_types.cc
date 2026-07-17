// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "ros2_types/util/ros2_type_support_factory.h"
#include "ros2_types/util/ros2_type_support.h"
#include "base/ibmw_config.h"

// Some ROS 2 generated std_msgs::msg::String aliases instantiate
// std::allocator<void>, which is not available with every target standard
// library. CMake probes the active ROS 2 headers/toolchain and enables this
// registrar only when that public alias is valid.
#if IBMW_ROS2_TYPES_HAS_STD_MSGS_STRING
#include <std_msgs/msg/string.hpp>
#endif
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/int64.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/bool.hpp>
#include <geometry_msgs/msg/point.hpp>
#include <geometry_msgs/msg/quaternion.hpp>
#include <geometry_msgs/msg/pose.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/vector3.hpp>

namespace ibmw::ros2 {

// Auto-register common ROS2 message types via static initialization

// ============================================================================
// std_msgs
// ============================================================================

#if IBMW_ROS2_TYPES_HAS_STD_MSGS_STRING
static TypeSupportRegistrar<std_msgs::msg::String> string_registrar(
    "std_msgs/msg/String",
    "std_msgs::msg::dds_::String_",
    false);  // String is not a plain type (contains dynamic string)
#endif

static TypeSupportRegistrar<std_msgs::msg::Int32> int32_registrar(
    "std_msgs/msg/Int32",
    "std_msgs::msg::dds_::Int32_",
    true);  // Int32 is a plain type

static TypeSupportRegistrar<std_msgs::msg::Int64> int64_registrar(
    "std_msgs/msg/Int64",
    "std_msgs::msg::dds_::Int64_",
    true);

static TypeSupportRegistrar<std_msgs::msg::Float32> float32_registrar(
    "std_msgs/msg/Float32",
    "std_msgs::msg::dds_::Float32_",
    true);

static TypeSupportRegistrar<std_msgs::msg::Float64> float64_registrar(
    "std_msgs/msg/Float64",
    "std_msgs::msg::dds_::Float64_",
    true);

static TypeSupportRegistrar<std_msgs::msg::Bool> bool_registrar(
    "std_msgs/msg/Bool",
    "std_msgs::msg::dds_::Bool_",
    true);

// ============================================================================
// geometry_msgs
// ============================================================================

static TypeSupportRegistrar<geometry_msgs::msg::Vector3> vector3_registrar(
    "geometry_msgs/msg/Vector3",
    "geometry_msgs::msg::dds_::Vector3_",
    true);  // Vector3 is a plain type (3 doubles)

static TypeSupportRegistrar<geometry_msgs::msg::Point> point_registrar(
    "geometry_msgs/msg/Point",
    "geometry_msgs::msg::dds_::Point_",
    true);

static TypeSupportRegistrar<geometry_msgs::msg::Quaternion> quaternion_registrar(
    "geometry_msgs/msg/Quaternion",
    "geometry_msgs::msg::dds_::Quaternion_",
    true);

static TypeSupportRegistrar<geometry_msgs::msg::Pose> pose_registrar(
    "geometry_msgs/msg/Pose",
    "geometry_msgs::msg::dds_::Pose_",
    true);

static TypeSupportRegistrar<geometry_msgs::msg::Twist> twist_registrar(
    "geometry_msgs/msg/Twist",
    "geometry_msgs::msg::dds_::Twist_",
    true);

}  // namespace ibmw::ros2
