// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// IBMW is licensed under Mulan PSL v2.

#pragma once

#include <memory>
#include <string>

#include "ros2_style_publisher.h"
#include "dds_transport_driver.h"

namespace ibmw::extensions::dds_extension {

/**
 * @brief Create a ROS2-style Publisher (factory function)
 * @param driver DDS Driver pointer
 * @param dds_topic_name DDS topic name (already converted)
 * @param config Zero-copy configuration
 */
template<typename MsgType>
Ros2StylePublisherPtr<MsgType> create_publisher(
    DdsTransportDriver* driver,
    const std::string& dds_topic_name,
    const ZeroCopyConfig& config = ZeroCopyConfig::Auto()) {

  if (driver == nullptr) {
    return nullptr;
  }

  auto* writer = driver->GetDataWriter(dds_topic_name);
  if (writer == nullptr) {
    return nullptr;
  }

  bool is_plain = driver->IsPlainType(dds_topic_name);

  return std::make_shared<Ros2StylePublisher<MsgType>>(
      writer, dds_topic_name, is_plain, config);
}

/**
 * @brief Create a ROS2-style Publisher (using Driver config)
 * @param driver DDS Driver pointer
 * @param ibmw_topic IBMW topic name (e.g. "point_topic")
 * @param msg_type Message type name (e.g. "geometry_msgs/msg/Point")
 */
template<typename MsgType>
Ros2StylePublisherPtr<MsgType> create_publisher_with_driver_config(
    DdsTransportDriver* driver,
    const std::string& ibmw_topic,
    const std::string& msg_type) {

  if (driver == nullptr) {
    return nullptr;
  }

  // Convert IBMW topic to DDS topic name based on ros2_compatible_mode
  std::string dds_topic_name;
  const auto& options = driver->GetOptions();

  if (options.ros2_compatible) {
    if (msg_type.find("geometry_msgs") == 0 ||
        msg_type.find("std_msgs") == 0 ||
        msg_type.find("sensor_msgs") == 0 ||
        msg_type.find("nav_msgs") == 0 ||
        msg_type.find("tf2_msgs") == 0) {
      dds_topic_name = options.ros2_topic_prefix + "/" + ibmw_topic;
    } else {
      dds_topic_name = ibmw_topic;
    }
  } else {
    dds_topic_name = ibmw_topic;
  }

  const auto& config = driver->GetZeroCopyConfig(dds_topic_name);
  return create_publisher<MsgType>(driver, dds_topic_name, config);
}

namespace ros2 {

template<typename MsgType>
using Publisher = Ros2StylePublisher<MsgType>;

template<typename MsgType>
using PublisherPtr = Ros2StylePublisherPtr<MsgType>;

}  // namespace ros2

}  // namespace ibmw::extensions::dds_extension
