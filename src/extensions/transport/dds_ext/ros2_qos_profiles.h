// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>

namespace ibmw::extensions::dds_extension {

/**
 * @brief ROS2 QoS preset types
 */
enum class Ros2QosPreset {
  DEFAULT,
  ROS2_SENSOR_DATA,
  ROS2_PARAMETERS,
  ROS2_SYSTEM_DEFAULT,
  ROS2_SERVICES_DEFAULT
};

/**
 * @brief Get ROS2 DataReader QoS
 */
inline eprosima::fastdds::dds::DataReaderQos GetRos2ReaderQos(Ros2QosPreset preset) {
  using namespace eprosima::fastdds::dds;
  DataReaderQos qos;

  switch (preset) {
    case Ros2QosPreset::ROS2_SENSOR_DATA:
      qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
      qos.durability().kind = VOLATILE_DURABILITY_QOS;
      qos.history().kind = KEEP_LAST_HISTORY_QOS;
      qos.history().depth = 5;
      break;
    case Ros2QosPreset::ROS2_PARAMETERS:
      qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
      qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
      qos.history().kind = KEEP_ALL_HISTORY_QOS;
      break;
    case Ros2QosPreset::ROS2_SYSTEM_DEFAULT:
    case Ros2QosPreset::ROS2_SERVICES_DEFAULT:
      qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
      qos.durability().kind = VOLATILE_DURABILITY_QOS;
      qos.history().kind = KEEP_LAST_HISTORY_QOS;
      qos.history().depth = 10;
      break;
    case Ros2QosPreset::DEFAULT:
    default:
      break;
  }

  return qos;
}

/**
 * @brief Get ROS2 DataWriter QoS
 */
inline eprosima::fastdds::dds::DataWriterQos GetRos2WriterQos(Ros2QosPreset preset) {
  using namespace eprosima::fastdds::dds;
  DataWriterQos qos;

  switch (preset) {
    case Ros2QosPreset::ROS2_SENSOR_DATA:
      qos.reliability().kind = BEST_EFFORT_RELIABILITY_QOS;
      qos.durability().kind = VOLATILE_DURABILITY_QOS;
      qos.history().kind = KEEP_LAST_HISTORY_QOS;
      qos.history().depth = 5;
      break;
    case Ros2QosPreset::ROS2_PARAMETERS:
      qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
      qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
      qos.history().kind = KEEP_ALL_HISTORY_QOS;
      break;
    case Ros2QosPreset::ROS2_SYSTEM_DEFAULT:
    case Ros2QosPreset::ROS2_SERVICES_DEFAULT:
      qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
      qos.durability().kind = VOLATILE_DURABILITY_QOS;
      qos.history().kind = KEEP_LAST_HISTORY_QOS;
      qos.history().depth = 10;
      break;
    case Ros2QosPreset::DEFAULT:
    default:
      break;
  }

  return qos;
}

/**
 * @brief Parse QoS preset string
 */
inline Ros2QosPreset ParseQosPreset(const std::string& preset_str) {
  if (preset_str == "ros2_sensor") return Ros2QosPreset::ROS2_SENSOR_DATA;
  if (preset_str == "ros2_parameters") return Ros2QosPreset::ROS2_PARAMETERS;
  if (preset_str == "ros2_system") return Ros2QosPreset::ROS2_SYSTEM_DEFAULT;
  if (preset_str == "ros2_services") return Ros2QosPreset::ROS2_SERVICES_DEFAULT;
  return Ros2QosPreset::DEFAULT;
}

/**
 * @brief Apply QoS preset to DataReaderQos
 */
inline void ApplyQosPreset(Ros2QosPreset preset,
                           eprosima::fastdds::dds::DataReaderQos& qos) {
  if (preset != Ros2QosPreset::DEFAULT) {
    auto preset_qos = GetRos2ReaderQos(preset);
    qos.reliability() = preset_qos.reliability();
    qos.durability() = preset_qos.durability();
    if (qos.history().kind == eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS &&
        qos.history().depth == 10) {
      qos.history() = preset_qos.history();
    }
  }
}

/**
 * @brief Apply QoS preset to DataWriterQos
 */
inline void ApplyQosPreset(Ros2QosPreset preset,
                           eprosima::fastdds::dds::DataWriterQos& qos) {
  if (preset != Ros2QosPreset::DEFAULT) {
    auto preset_qos = GetRos2WriterQos(preset);
    qos.reliability() = preset_qos.reliability();
    qos.durability() = preset_qos.durability();
    if (qos.history().kind == eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS &&
        qos.history().depth == 10) {
      qos.history() = preset_qos.history();
    }
  }
}

}  // namespace ibmw::extensions::dds_extension
