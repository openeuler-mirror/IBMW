// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstring>
#include <functional>
#include <string>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

// v2-to-v1 compat: provides CdrSizeCalculator, CdrVersion, MemberId, etc.
#include "dds_types/util/fastcdr_v1_compat.h"

#ifndef IBMW_DDS_HAS_MODERN_FASTDDS
#if __has_include(<fastdds/config.hpp>)
#include <fastdds/config.hpp>
#define IBMW_DDS_HAS_MODERN_FASTDDS 1
#else
#define IBMW_DDS_HAS_MODERN_FASTDDS 0
#endif
#endif

#include <fastdds/dds/topic/TopicDataType.hpp>
#if IBMW_DDS_HAS_MODERN_FASTDDS
#include <fastdds/rtps/common/InstanceHandle.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>
namespace ibmw::dds {
namespace rtps_compat = eprosima::fastdds::rtps;
}  // namespace ibmw::dds
#else
#include <fastrtps/rtps/common/InstanceHandle.h>
#include <fastrtps/rtps/common/SerializedPayload.h>
namespace ibmw::dds {
namespace rtps_compat = eprosima::fastrtps::rtps;
}  // namespace ibmw::dds
#endif

namespace ibmw::dds {

/**
 * @brief Generic FastDDS TopicDataType adapter for DDS IDL types.
 *
 * This template class bridges between FastDDS 2.x TopicDataType interface
 * and FastCDR v1 serialize/deserialize via operator<< / operator>>.
 *
 * The DDS transport driver requires a non-null custom_type_support_ptr
 * to create DataWriters/DataReaders. This class provides that.
 *
 * @tparam DdsType The DDS IDL type (e.g., ibmw::protocols::example::ExampleTopic)
 *                 Must have operator<< / operator>> defined for eprosima::fastcdr::Cdr.
 */
template <typename DdsType>
class GenericDdsPubSubType : public eprosima::fastdds::dds::TopicDataType {
 public:
  /**
   * @brief Constructor
   * @param type_name The DDS topic type name (bare name, no "dds:" prefix)
   */
  explicit GenericDdsPubSubType(const std::string& type_name) {
#if IBMW_DDS_HAS_MODERN_FASTDDS
    set_name(type_name);
#else
    setName(type_name.c_str());
#endif

    // Estimate max serialized size from a default-constructed instance.
    // Uses our compat CdrSizeCalculator.
    DdsType default_instance{};
    eprosima::fastcdr::CdrSizeCalculator calculator(
        eprosima::fastcdr::CdrVersion::XCDRv2);
    size_t current_alignment = 0;
    size_t base_size =
        calculator.calculate_serialized_size(default_instance, current_alignment);
    // Add encapsulation header (4 bytes) and padding
    const auto type_size = static_cast<uint32_t>(base_size) + 4u;
#if IBMW_DDS_HAS_MODERN_FASTDDS
    max_serialized_type_size = type_size;
#else
    m_typeSize = type_size;
#endif
    // Minimum size to avoid zero-allocation issues
    if (type_size < 64u) {
#if IBMW_DDS_HAS_MODERN_FASTDDS
      max_serialized_type_size = 64u;
#else
      m_typeSize = 64u;
#endif
    }

#if IBMW_DDS_HAS_MODERN_FASTDDS
    is_compute_key_provided = false;
#else
    m_isGetKeyDefined = false;
    auto_fill_type_object(false);
    auto_fill_type_information(false);
#endif
  }

  ~GenericDdsPubSubType() override = default;

  // ========== Serialization ==========

#if IBMW_DDS_HAS_MODERN_FASTDDS
  bool serialize(
      const void* const data,
      rtps_compat::SerializedPayload_t& payload,
      eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
    static_cast<void>(data_representation);
    const auto* typed_data = static_cast<const DdsType*>(data);
#else
  bool serialize(
      void* data,
      rtps_compat::SerializedPayload_t* payload_ptr) override {
    auto* typed_data = static_cast<DdsType*>(data);
    auto& payload = *payload_ptr;
#endif

    // Estimate serialized size
    eprosima::fastcdr::CdrSizeCalculator calculator(
        eprosima::fastcdr::CdrVersion::XCDRv2);
    size_t current_alignment = 0;
    size_t data_size =
        calculator.calculate_serialized_size(*typed_data, current_alignment);
    // Total = encapsulation header (4 bytes) + data
    uint32_t total_size = static_cast<uint32_t>(data_size) + 4u;

    // Ensure payload has enough space
    if (payload.max_size < total_size) {
      payload.reserve(total_size);
    }

    try {
      eprosima::fastcdr::FastBuffer fast_buffer(
          reinterpret_cast<char*>(payload.data), payload.max_size);
      eprosima::fastcdr::Cdr cdr(
          fast_buffer,
          eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
#if defined(FASTCDR_VERSION_MAJOR) && FASTCDR_VERSION_MAJOR >= 2
          eprosima::fastcdr::DDS_CDR);
#else
          eprosima::fastcdr::Cdr::DDS_CDR);
#endif

      // Write encapsulation header
      cdr.serialize_encapsulation();

      // Serialize using the explicit free-function specialization
      // (not operator<< which in v1 calls data.serialize(cdr) member function)
      eprosima::fastcdr::serialize(cdr, *typed_data);

      payload.length = static_cast<uint32_t>(
          ibmw::fastcdr_compat::get_serialized_data_length(cdr));
      payload.encapsulation = cdr.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS
                                    ? CDR_BE
                                    : CDR_LE;
    } catch (...) {
      return false;
    }

    return true;
  }

#if IBMW_DDS_HAS_MODERN_FASTDDS
  bool deserialize(
      rtps_compat::SerializedPayload_t& payload,
      void* data) override {
#else
  bool deserialize(
      rtps_compat::SerializedPayload_t* payload_ptr,
      void* data) override {
    auto& payload = *payload_ptr;
#endif
    auto* typed_data = static_cast<DdsType*>(data);

    try {
      eprosima::fastcdr::FastBuffer fast_buffer(
          reinterpret_cast<char*>(payload.data), payload.length);
      eprosima::fastcdr::Cdr cdr(
          fast_buffer,
          eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
#if defined(FASTCDR_VERSION_MAJOR) && FASTCDR_VERSION_MAJOR >= 2
          eprosima::fastcdr::DDS_CDR);
#else
          eprosima::fastcdr::Cdr::DDS_CDR);
#endif

      // Read encapsulation header
      cdr.read_encapsulation();

      // Deserialize using the explicit free-function specialization
      // (not operator>> which in v1 calls data.deserialize(cdr) member function)
      eprosima::fastcdr::deserialize(cdr, *typed_data);
    } catch (...) {
      return false;
    }

    return true;
  }

  // ========== Size provider ==========

  static uint32_t EstimateSerializedSize(const DdsType& data) {
      eprosima::fastcdr::CdrSizeCalculator calculator(
          eprosima::fastcdr::CdrVersion::XCDRv2);
      size_t current_alignment = 0;
      size_t data_size =
          calculator.calculate_serialized_size(data, current_alignment);
      return static_cast<uint32_t>(data_size) + 4u;  // + encapsulation header
  }

#if IBMW_DDS_HAS_MODERN_FASTDDS
  uint32_t calculate_serialized_size(
      const void* const data,
      eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
    static_cast<void>(data_representation);
    return EstimateSerializedSize(*static_cast<const DdsType*>(data));
  }

  void* create_data() override {
    return new DdsType();
  }

  void delete_data(void* data) override {
    delete static_cast<DdsType*>(data);
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
  std::function<uint32_t()> getSerializedSizeProvider(void* data) override {
    auto* typed_data = static_cast<DdsType*>(data);
    return [typed_data]() -> uint32_t {
      return EstimateSerializedSize(*typed_data);
    };
  }

  // ========== Memory management ==========

  void* createData() override {
    return new DdsType();
  }

  void deleteData(void* data) override {
    delete static_cast<DdsType*>(data);
  }

  // ========== Key support (not used for keyless types) ==========

  bool getKey(
      void* /*data*/,
      rtps_compat::InstanceHandle_t* /*ihandle*/,
      bool /*force_md5*/) override {
    return false;
  }
#endif
};

}  // namespace ibmw::dds
