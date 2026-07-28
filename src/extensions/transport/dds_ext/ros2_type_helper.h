// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <algorithm>
#include <string>

namespace ibmw::extensions::dds_extension {

/**
 * @brief ROS2 type detection and conversion utilities
 */
class ROS2TypeHelper {
 public:
  /**
   * @brief Detect if a message type is ROS2
   * @param msg_type ibmw message type name (e.g., "ros2:std_msgs/msg/String")
   * @return true if it's a ROS2 type
   */
  static bool IsROS2Type(const std::string& msg_type) {
    return msg_type.find("ros2:") == 0;
  }

  /**
   * @brief Extract ROS2 type name from ibmw type name
   * @param msg_type ibmw message type name (e.g., "ros2:std_msgs/msg/String")
   * @return ROS2 type name (e.g., "std_msgs/msg/String")
   */
  static std::string ExtractROS2TypeName(const std::string& msg_type) {
    if (IsROS2Type(msg_type)) {
      return msg_type.substr(5);  // Remove "ros2:" prefix
    }
    return msg_type;
  }

  /**
   * @brief Convert ROS2 type name to DDS type name
   * @param ros2_type ROS2 type name (e.g., "std_msgs/msg/String")
   * @return DDS type name (e.g., "std_msgs::msg::dds_::String_")
   */
  static std::string ConvertToDdsTypeName(const std::string& ros2_type) {
    const size_t last_sep = ros2_type.rfind('/');
    if (last_sep != std::string::npos) {
      std::string dds_type;
      dds_type.reserve(ros2_type.size() + 8);
      for (size_t index = 0; index < last_sep; ++index) {
        if (ros2_type[index] == '/') {
          dds_type += "::";
        } else {
          dds_type += ros2_type[index];
        }
      }
      dds_type += "::dds_::";
      dds_type.append(ros2_type, last_sep + 1, std::string::npos);
      dds_type += '_';
      return dds_type;
    }
    return ros2_type;
  }

  /**
   * @brief Convert ROS2 topic name to DDS topic name
   * @param ros2_topic ROS2 topic name (e.g., "/chatter")
   * @param ros2_topic_prefix ROS2 topic prefix (e.g., "rt")
   * @return DDS topic name (e.g., "rt/chatter")
   */
  static std::string ConvertToDdsTopicName(
      const std::string& ros2_topic,
      const std::string& ros2_topic_prefix) {

    std::string dds_topic = ros2_topic;

    // Remove leading slash
    if (!dds_topic.empty() && dds_topic[0] == '/') {
      dds_topic = dds_topic.substr(1);
    }

    // Add ROS2 prefix
    if (!ros2_topic_prefix.empty()) {
      return ros2_topic_prefix + "/" + dds_topic;
    }

    return dds_topic;
  }
};

}  // namespace ibmw::extensions::dds_extension
