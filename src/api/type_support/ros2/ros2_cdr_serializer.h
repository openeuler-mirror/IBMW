// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstring>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <stdexcept>

#if __has_include(<fastdds/config.hpp>)
#include <fastdds/config.hpp>
#define IBMW_ROS2_HAS_MODERN_FASTDDS 1
#else
#define IBMW_ROS2_HAS_MODERN_FASTDDS 0
#endif

#if __has_include(<fastcdr/config.h>)
#include <fastcdr/config.h>
#endif

// Include FastDDS/FastRTPS headers FIRST (they may define MEMBER_ID_INVALID macro)
#if IBMW_ROS2_HAS_MODERN_FASTDDS
#if __has_include(<fastdds/rtps/common/SerializedPayload.hpp>)
#include <fastdds/rtps/common/SerializedPayload.hpp>
#endif
#if __has_include(<fastdds/rtps/common/InstanceHandle.hpp>)
#include <fastdds/rtps/common/InstanceHandle.hpp>
#endif
namespace ibmw::ros2 {
namespace rtps_compat = eprosima::fastdds::rtps;
}  // namespace ibmw::ros2
#else
#if __has_include(<fastrtps/rtps/common/SerializedPayload.h>)
#include <fastrtps/rtps/common/SerializedPayload.h>
#endif
#if __has_include(<fastrtps/rtps/common/InstanceHandle.h>)
#include <fastrtps/rtps/common/InstanceHandle.h>
#endif
namespace ibmw::ros2 {
namespace rtps_compat = eprosima::fastrtps::rtps;
}  // namespace ibmw::ros2
#endif

// Fix conflict: ROS Humble's FastRTPS TypesBase.h defines MEMBER_ID_INVALID as
// a macro (#define MEMBER_ID_INVALID 0X0FFFFFFF), which clashes with other code.
#if !IBMW_ROS2_HAS_MODERN_FASTDDS
#ifdef MEMBER_ID_INVALID
#undef MEMBER_ID_INVALID
#endif
#endif

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

#if __has_include(<rosidl_typesupport_fastrtps_cpp/identifier.hpp>) && \
    __has_include(<rosidl_typesupport_fastrtps_cpp/message_type_support.h>) && \
    __has_include(<rosidl_typesupport_fastrtps_cpp/message_type_support_decl.hpp>)
#include <rosidl_typesupport_fastrtps_cpp/identifier.hpp>
#include <rosidl_typesupport_fastrtps_cpp/message_type_support.h>
#include <rosidl_typesupport_fastrtps_cpp/message_type_support_decl.hpp>
#define IBMW_ROS2_HAS_FASTRTPS_CPP_CALLBACKS 1
#else
struct message_type_support_callbacks_t;
#define IBMW_ROS2_HAS_FASTRTPS_CPP_CALLBACKS 0
#endif

namespace ibmw::ros2 {

/**
 * @brief ROS2 message CDR serializer (no RMW dependency)
 *
 * Uses FastCDR directly for ROS2 standard CDR format serialization.
 * No dependency on rclcpp, rmw_fastrtps_cpp, or other ROS2 runtime libraries.
 */
class ROS2CdrSerializer {
 public:
  static constexpr auto kCdrType =
#if defined(FASTCDR_VERSION_MAJOR) && (FASTCDR_VERSION_MAJOR >= 2)
      eprosima::fastcdr::DDS_CDR;
#else
      eprosima::fastcdr::Cdr::DDS_CDR;
#endif

  template <typename ROS2MsgType>
  static bool Serialize(
      const ROS2MsgType& msg,
      rtps_compat::SerializedPayload_t* payload) {
    try {
      uint32_t estimated_size = EstimateSize(msg);

      if (payload->max_size < estimated_size) {
        payload->reserve(estimated_size);
      }

      eprosima::fastcdr::FastBuffer fastbuffer(
          reinterpret_cast<char*>(payload->data),
          payload->max_size);

      eprosima::fastcdr::Cdr ser(
          fastbuffer,
          eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
          kCdrType);

      ser.serialize_encapsulation();
      SerializeMessage(ser, msg);
      payload->length = static_cast<uint32_t>(
#if defined(FASTCDR_VERSION_MAJOR) && (FASTCDR_VERSION_MAJOR >= 2)
          ser.get_serialized_data_length());
#else
          ser.getSerializedDataLength());
#endif

      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  template <typename ROS2MsgType>
  static bool Deserialize(
      rtps_compat::SerializedPayload_t* payload,
      ROS2MsgType& msg) {
    try {
      eprosima::fastcdr::FastBuffer fastbuffer(
          reinterpret_cast<char*>(payload->data),
          payload->length);

      eprosima::fastcdr::Cdr deser(
          fastbuffer,
          eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
          kCdrType);

      deser.read_encapsulation();
      DeserializeMessage(deser, msg);

      return true;
    } catch (const std::exception&) {
      return false;
    }
  }

  template <typename ROS2MsgType>
  static uint32_t EstimateSize(const ROS2MsgType& msg) {
#if IBMW_ROS2_HAS_FASTRTPS_CPP_CALLBACKS
    if (const auto* callbacks = GetFastRtpsCallbacks<ROS2MsgType>()) {
      return 4U + callbacks->get_serialized_size(&msg);  // encapsulation + ROS2 payload
    }
#else
    static_cast<void>(msg);
#endif
    throw std::runtime_error(
        "ROS2CdrSerializer::EstimateSize: no generated FastRTPS callbacks for this type. "
        "DDS transport requires rosidl_typesupport_fastrtps_cpp callbacks or an explicit per-message serializer; "
        "refusing to guess serialized size.");
  }

  template <typename ROS2MsgType>
  static void SerializeMessage(eprosima::fastcdr::Cdr& cdr, const ROS2MsgType& msg) {
#if IBMW_ROS2_HAS_FASTRTPS_CPP_CALLBACKS
    if (const auto* callbacks = GetFastRtpsCallbacks<ROS2MsgType>()) {
      if (!callbacks->cdr_serialize(&msg, cdr)) {
        throw std::runtime_error("ROS2 FastRTPS generated cdr_serialize callback failed");
      }
      return;
    }
#else
    static_cast<void>(cdr);
    static_cast<void>(msg);
#endif
    throw std::runtime_error(
        "ROS2CdrSerializer::SerializeMessage: no specialization for this type. "
        "DDS transport requires generated FastRTPS callbacks or explicit CDR serialization support.");
  }

  template <typename ROS2MsgType>
  static void DeserializeMessage(eprosima::fastcdr::Cdr& cdr, ROS2MsgType& msg) {
#if IBMW_ROS2_HAS_FASTRTPS_CPP_CALLBACKS
    if (const auto* callbacks = GetFastRtpsCallbacks<ROS2MsgType>()) {
      if (!callbacks->cdr_deserialize(cdr, &msg)) {
        throw std::runtime_error("ROS2 FastRTPS generated cdr_deserialize callback failed");
      }
      return;
    }
#else
    static_cast<void>(cdr);
    static_cast<void>(msg);
#endif
    throw std::runtime_error(
        "ROS2CdrSerializer::DeserializeMessage: no specialization for this type. "
        "DDS transport requires generated FastRTPS callbacks or explicit CDR serialization support.");
  }

  // Largest serialized payload including the encapsulation header, from the
  // generated FastRTPS callbacks. 0 when the type has unbounded members or no
  // callbacks are available.
  template <typename ROS2MsgType>
  static uint32_t MaxSerializedSize() {
#if IBMW_ROS2_HAS_FASTRTPS_CPP_CALLBACKS && defined(ROSIDL_TYPESUPPORT_FASTRTPS_UNBOUNDED_TYPE)
    const auto* callbacks = GetFastRtpsCallbacks<ROS2MsgType>();
    if (callbacks != nullptr && callbacks->max_serialized_size != nullptr) {
      char bounds_info = ROSIDL_TYPESUPPORT_FASTRTPS_UNBOUNDED_TYPE;
      const size_t size = callbacks->max_serialized_size(bounds_info);
      if (bounds_info != ROSIDL_TYPESUPPORT_FASTRTPS_UNBOUNDED_TYPE &&
          size <= std::numeric_limits<uint32_t>::max() - 4U) {
        return 4U + static_cast<uint32_t>(size);  // encapsulation + ROS2 payload
      }
    }
#endif
    return 0;
  }

 private:
  template <typename ROS2MsgType>
  static const message_type_support_callbacks_t* GetFastRtpsCallbacks() {
#if IBMW_ROS2_HAS_FASTRTPS_CPP_CALLBACKS
    const rosidl_message_type_support_t* type_support =
        rosidl_typesupport_fastrtps_cpp::get_message_type_support_handle<ROS2MsgType>();
    if (type_support == nullptr || type_support->data == nullptr ||
        type_support->typesupport_identifier == nullptr) {
      return nullptr;
    }
    if (std::strcmp(
            type_support->typesupport_identifier,
            rosidl_typesupport_fastrtps_cpp::typesupport_identifier) != 0) {
      return nullptr;
    }
    return static_cast<const message_type_support_callbacks_t*>(type_support->data);
#else
    return nullptr;
#endif
  }
};

}  // namespace ibmw::ros2
