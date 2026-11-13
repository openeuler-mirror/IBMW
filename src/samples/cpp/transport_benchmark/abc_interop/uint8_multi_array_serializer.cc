// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "uint8_multi_array_serializer.h"

#include <limits>
#include <stdexcept>

namespace ibmw::ros2 {

template <>
bool ROS2CdrSerializer::Serialize<std_msgs::msg::UInt8MultiArray>(
    const std_msgs::msg::UInt8MultiArray& msg,
    rtps_compat::SerializedPayload_t* payload) {
  try {
    const uint32_t estimated_size = EstimateSize(msg);
    if (payload->max_size < estimated_size) {
      payload->reserve(estimated_size);
      if (payload->data == nullptr || payload->max_size < estimated_size) {
        return false;
      }
    }

    eprosima::fastcdr::FastBuffer fastbuffer(
        reinterpret_cast<char*>(payload->data), payload->max_size);
    eprosima::fastcdr::Cdr cdr(
        fastbuffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, kCdrType);
    cdr.serialize_encapsulation();
    SerializeMessage(cdr, msg);
    payload->length = static_cast<uint32_t>(
#if defined(FASTCDR_VERSION_MAJOR) && (FASTCDR_VERSION_MAJOR >= 2)
        cdr.get_serialized_data_length());
#else
        cdr.getSerializedDataLength());
#endif
    payload->encapsulation =
        cdr.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS
            ? CDR_BE
            : CDR_LE;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

template <>
uint32_t ROS2CdrSerializer::EstimateSize(
    const std_msgs::msg::UInt8MultiArray& msg) {
  // Includes encapsulation and conservative alignment allowance for each field.
  size_t size = 20U + msg.data.size();
  for (const auto& dim : msg.layout.dim) {
    size += 20U + dim.label.size();
  }
  if (size > std::numeric_limits<uint32_t>::max()) {
    throw std::length_error("UInt8MultiArray exceeds the DDS payload size limit");
  }
  return static_cast<uint32_t>(size);
}

template <>
void ROS2CdrSerializer::SerializeMessage(
    eprosima::fastcdr::Cdr& cdr,
    const std_msgs::msg::UInt8MultiArray& msg) {
  cdr << static_cast<uint32_t>(msg.layout.dim.size());
  for (const auto& dim : msg.layout.dim) {
    cdr << dim.label;
    cdr << dim.size;
    cdr << dim.stride;
  }
  cdr << msg.layout.data_offset;
  cdr << msg.data;
}

template <>
void ROS2CdrSerializer::DeserializeMessage(
    eprosima::fastcdr::Cdr& cdr,
    std_msgs::msg::UInt8MultiArray& msg) {
  uint32_t dimension_count = 0;
  cdr >> dimension_count;
  msg.layout.dim.resize(dimension_count);
  for (auto& dim : msg.layout.dim) {
    cdr >> dim.label;
    cdr >> dim.size;
    cdr >> dim.stride;
  }
  cdr >> msg.layout.data_offset;
  cdr >> msg.data;
}

}  // namespace ibmw::ros2
