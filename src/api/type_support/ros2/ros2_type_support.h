// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>
#include <type_traits>

#if __has_include(<fastdds/config.hpp>)
#include <fastdds/config.hpp>
#define IBMW_ROS2_HAS_MODERN_FASTDDS 1
#else
#define IBMW_ROS2_HAS_MODERN_FASTDDS 0
#endif

#include <fastdds/dds/topic/TopicDataType.hpp>

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
// a macro, which clashes with FastCDR v2's MemberId.hpp static const. Undefine
// before FastCDR v2 headers are pulled in via ros2_cdr_serializer.h.
#ifdef MEMBER_ID_INVALID
#undef MEMBER_ID_INVALID
#endif

#include "ros2_cdr_serializer.h"
#include "ros2_cdr_serializer_impl.h"

namespace ibmw::ros2 {

/**
 * @brief FastDDS TopicDataType implementation for ROS2 messages (no RMW dependency)
 *
 * @tparam ROS2MsgType ROS2 message type (e.g., std_msgs::msg::String)
 *
 * Features:
 * - No RMW layer dependency (rmw_fastrtps_cpp)
 * - Direct FastCDR serialization
 * - Plain type optimization support
 */
template <typename ROS2MsgType>
class ROS2TypeSupport : public eprosima::fastdds::dds::TopicDataType {
 public:
  static constexpr uint32_t kDynamicSerializedTypeSize = 1024U * 1024U;

  /**
   * @brief Constructor
   *
   * @param type_name DDS type name (e.g., "std_msgs::msg::dds_::String_")
   * @param is_plain Whether this is a plain type (fixed size, no dynamic fields)
   */
  explicit ROS2TypeSupport(const std::string& type_name, bool is_plain = false)
      : is_plain_(is_plain) {
    const uint32_t serialized_type_size = CalculateMaxSerializedSize();
    const uint32_t type_size = serialized_type_size != 0 ? serialized_type_size : kDynamicSerializedTypeSize;
#if IBMW_ROS2_HAS_MODERN_FASTDDS
    set_name(type_name);
    max_serialized_type_size = type_size;
    is_compute_key_provided = false;
#else
    setName(type_name.c_str());
    m_typeSize = type_size;

    m_isGetKeyDefined = false;
#endif
  }

  // ========== Core serialization interface ==========

#if IBMW_ROS2_HAS_MODERN_FASTDDS
  bool serialize(
      const void* const data,
      rtps_compat::SerializedPayload_t& payload,
      eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
    static_cast<void>(data_representation);
    auto* msg = static_cast<const ROS2MsgType*>(data);
    return ROS2CdrSerializer::Serialize(*msg, &payload);
  }

  bool deserialize(
      rtps_compat::SerializedPayload_t& payload,
      void* data) override {
    auto* msg = static_cast<ROS2MsgType*>(data);
    return ROS2CdrSerializer::Deserialize(&payload, *msg);
  }

  uint32_t calculate_serialized_size(
      const void* const data,
      eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
    static_cast<void>(data_representation);
    const auto* msg = static_cast<const ROS2MsgType*>(data);
    return ROS2CdrSerializer::EstimateSize(*msg);
  }

  void* create_data() override {
    return new ROS2MsgType();
  }

  void delete_data(void* data) override {
    delete static_cast<ROS2MsgType*>(data);
  }

  bool compute_key(
      rtps_compat::SerializedPayload_t& /*payload*/,
      rtps_compat::InstanceHandle_t& /*ihandle*/,
      bool /*force_md5*/ = false) override {
    return false;
  }

  bool compute_key(
      const void* const /*data*/,
      rtps_compat::InstanceHandle_t& /*ihandle*/,
      bool /*force_md5*/ = false) override {
    return false;
  }

#else
  bool serialize(
      void* data,
      rtps_compat::SerializedPayload_t* payload) override {
    auto* msg = static_cast<ROS2MsgType*>(data);
    return ROS2CdrSerializer::Serialize(*msg, payload);
  }

  bool deserialize(
      rtps_compat::SerializedPayload_t* payload,
      void* data) override {
    auto* msg = static_cast<ROS2MsgType*>(data);
    return ROS2CdrSerializer::Deserialize(payload, *msg);
  }

  std::function<uint32_t()> getSerializedSizeProvider(void* data) override {
    auto* msg = static_cast<ROS2MsgType*>(data);
    return [msg]() -> uint32_t {
      return ROS2CdrSerializer::EstimateSize(*msg);
    };
  }

  // ========== Memory management interface ==========

  void* createData() override {
    return new ROS2MsgType();
  }

  void deleteData(void* data) override {
    delete static_cast<ROS2MsgType*>(data);
  }

  // ========== Key support (ROS2 typically does not use keys) ==========

  bool getKey(
      void* /*data*/,
      rtps_compat::InstanceHandle_t* /*ihandle*/,
      bool /*force_md5*/ = false) override {
    return false;
  }
#endif

  // ========== Type traits ==========

  bool is_plain() const {
    return is_plain_;
  }

  bool is_bounded() const {
    return is_plain_;
  }

 private:
  bool is_plain_;

  uint32_t CalculateMaxSerializedSize() const {
    if constexpr (std::is_standard_layout_v<ROS2MsgType> &&
                  std::is_trivially_copyable_v<ROS2MsgType>) {
      return 4 + sizeof(ROS2MsgType);  // encapsulation header + struct size
    }
    return 0;
  }
};

}  // namespace ibmw::ros2
