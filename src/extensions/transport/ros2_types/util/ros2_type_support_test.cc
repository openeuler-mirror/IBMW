// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// API-035: the DDS max serialized size comes from the generated CDR bound,
// not from the in-memory struct size.

#include <gtest/gtest.h>

#include <geometry_msgs/msg/pose.hpp>
#include <rosidl_typesupport_fastrtps_cpp/message_type_support.h>
#include <rosidl_typesupport_fastrtps_cpp/message_type_support_decl.hpp>
#include <sensor_msgs/msg/region_of_interest.hpp>
#include <std_msgs/msg/string.hpp>

#include "type_support/ros2/ros2_type_support.h"

namespace ibmw::ros2 {
namespace {

template <typename T>
uint32_t TypeSize(const ROS2TypeSupport<T>& type_support) {
#if IBMW_ROS2_HAS_MODERN_FASTDDS
  return type_support.max_serialized_type_size;
#else
  return type_support.m_typeSize;
#endif
}

template <typename T>
size_t GeneratedBound(char& bounds_info) {
  const auto* handle = rosidl_typesupport_fastrtps_cpp::get_message_type_support_handle<T>();
  const auto* callbacks = static_cast<const message_type_support_callbacks_t*>(handle->data);
  return callbacks->max_serialized_size(bounds_info);
}

template <typename T>
void ExpectGeneratedBound(const T& max_value) {
  char bounds_info = 0;
  const size_t bound = GeneratedBound<T>(bounds_info);
  ASSERT_NE(bounds_info, ROSIDL_TYPESUPPORT_FASTRTPS_UNBOUNDED_TYPE);

  ROS2TypeSupport<T> type_support("t");
  EXPECT_EQ(TypeSize(type_support), 4u + bound);

  rtps_compat::SerializedPayload_t payload(TypeSize(type_support));
  T copy = max_value;
#if IBMW_ROS2_HAS_MODERN_FASTDDS
  ASSERT_TRUE(type_support.serialize(&copy, payload, eprosima::fastdds::dds::DEFAULT_DATA_REPRESENTATION));
#else
  ASSERT_TRUE(type_support.serialize(&copy, &payload));
#endif
  EXPECT_LE(payload.length, TypeSize(type_support));
}

TEST(Ros2TypeSupportTest, PlainTypesUseTheGeneratedCdrBound) {
  geometry_msgs::msg::Pose pose;
  pose.position.x = 1;
  ExpectGeneratedBound(pose);

  // Padding makes sizeof (20) differ from the CDR size (17).
  sensor_msgs::msg::RegionOfInterest roi;
  roi.width = 0xffffffff;
  roi.do_rectify = true;
  ExpectGeneratedBound(roi);
}

// The DDS driver's plain loan path copies type_size - 4 bytes of the struct.
TEST(Ros2TypeSupportTest, PlainLoansKeepTheInMemorySize) {
  ROS2TypeSupport<sensor_msgs::msg::RegionOfInterest> type_support("t", true);
  EXPECT_EQ(TypeSize(type_support), 4u + sizeof(sensor_msgs::msg::RegionOfInterest));
}

TEST(Ros2TypeSupportTest, UnboundedTypesUseTheDynamicDefault) {
  ROS2TypeSupport<std_msgs::msg::String> type_support("t");
  EXPECT_EQ(TypeSize(type_support), ROS2TypeSupport<std_msgs::msg::String>::kDynamicSerializedTypeSize);
}

}  // namespace
}  // namespace ibmw::ros2
