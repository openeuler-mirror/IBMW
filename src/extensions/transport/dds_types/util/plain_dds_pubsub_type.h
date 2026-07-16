// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstring>
#include <functional>
#include <string>

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

// ---------------------------------------------------------------------------
// PlainDdsPubSubType<T>
//
// A FastDDS TopicDataType implementation for "Plain" (POD-like, fixed-size,
// trivially-copyable) C++ types. Sister of GenericDdsPubSubType<T> in
// generic_dds_pubsub_type.h, but fundamentally different in two ways:
//
//   1. is_plain() is overridden to return true. This is the *only* way
//      FastDDS will engage the LoanedMessage / DataSharing zero-copy
//      transport path. Without this, even a perfectly trivially-copyable
//      C++ type silently degrades to the serialize -> network -> deserialize
//      copy path (regardless of writer_qos.data_sharing().automatic()).
//
//   2. serialize / deserialize / size-provider use raw `memcpy` over
//      `sizeof(T)` plus a 4-byte CDR encapsulation header. NO FastCDR
//      calculator, NO operator<< / operator>>, NO per-member iteration.
//      This is the contract the LoanedMessage path expects from a Plain
//      type: in-memory layout == on-wire layout (after the 4-byte
//      encapsulation prefix), so the SHM chunk produced by the writer
//      can be handed straight to the reader.
//
// Performance discipline (zero-copy first, no extra memcpy):
//   The DataSharing / LoanedMessage path *does not call serialize() at
//   runtime when the writer and reader are both in the same SHM segment.*
//   In that case the user's `borrow_loaned_message() -> write fields ->
//   publish(std::move)` is the ONLY data motion. serialize() / deserialize()
//   here exist solely to satisfy the TopicDataType contract for cross-
//   process / cross-host fallback (UDP, TCP, etc.); on those paths the
//   memcpy is unavoidable -- there's no FastCDR step left to remove.
//
// Type contract (compile-time enforced):
//   - std::is_trivially_copyable_v<T> must hold (a wire-compat memcpy
//     would corrupt non-POD types).
//   - sizeof(T) is the on-wire size after the 4-byte encap header.
//
// Topic / DataWriter wiring (runtime):
//   - m_typeSize = sizeof(T) + 4. FastDDS uses this to size SHM chunks
//     and transport buffers; over-reporting wastes SHM, under-reporting
//     truncates the payload. + 4 covers the encapsulation header that
//     serialize() prepends.
//   - m_isGetKeyDefined = false. Bench types are keyless.
//   - auto_fill_type_object / auto_fill_type_information = false. We
//     don't depend on FastDDS' XCDR2 type-object machinery; the wire
//     contract is "memcpy of sizeof(T) bytes" and is fully described by
//     the type name match between writer and reader.

template <typename T>
class PlainDdsPubSubType : public eprosima::fastdds::dds::TopicDataType {
 public:
  static_assert(std::is_trivially_copyable_v<T>,
                "PlainDdsPubSubType requires trivially-copyable T");

  // 4-byte CDR encapsulation header that prepends the raw bytes on the
  // wire (CDR_LE / CDR_BE + 2 reserved). Must match what
  // GenericDdsPubSubType uses so a Plain writer can interop with a
  // non-plain reader (the generic deserializer skips the same 4 bytes).
  static constexpr uint32_t kEncapsulationHeaderBytes = 4u;
  static constexpr uint32_t kFixedSerializedSize =
      static_cast<uint32_t>(sizeof(T)) + kEncapsulationHeaderBytes;

  explicit PlainDdsPubSubType(const std::string& type_name) {
#if IBMW_DDS_HAS_MODERN_FASTDDS
    set_name(type_name);
    max_serialized_type_size = kFixedSerializedSize;
    is_compute_key_provided = false;
#else
    setName(type_name.c_str());
    m_typeSize = kFixedSerializedSize;
    m_isGetKeyDefined = false;
    auto_fill_type_object(false);
    auto_fill_type_information(false);
#endif
  }

  ~PlainDdsPubSubType() override = default;

  // ========== Plain-ness signal: the whole point of this class. ==========
  // FastDDS 2.6 (ROS Humble bundle) ships a single no-arg is_plain()
  // virtual on TopicDataType. Newer Fast-DDS versions add a
  // DataRepresentationId_t overload; if/when we move to one of those,
  // mirror this override with the same `return true;` body.
#if IBMW_DDS_HAS_MODERN_FASTDDS
  bool is_plain(eprosima::fastdds::dds::DataRepresentationId_t) const override {
    return true;
  }
#else
  bool is_plain() const override { return true; }
#endif

  // ========== Boundedness signal (DataSharing prerequisite). ==========
  // FastDDS 2.6 only engages the DataSharing transport when both
  // is_plain() AND is_bounded() return true on the registered
  // TopicDataType (TypeSupport.hpp:243 + TopicDataType.hpp:303). The
  // default impl returns false, which silently demotes the QoS request
  // (writer_qos.data_sharing().automatic()) to SharedMemoryTransport --
  // exactly the trap the 6.2.7.5 audit hit. sizeof(T) is a compile-time
  // constant for every IBMW_DDS_PLAIN_TYPE, so the type is literally
  // bounded by m_typeSize.
  bool is_bounded() const override { return true; }

  // ========== Serialization (cross-host fallback only; SHM path skips this) ==
#if IBMW_DDS_HAS_MODERN_FASTDDS
  bool serialize(
      const void* const data,
      rtps_compat::SerializedPayload_t& payload,
      eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
    static_cast<void>(data_representation);
#else
  bool serialize(
      void* data,
      rtps_compat::SerializedPayload_t* payload_ptr) override {
    auto& payload = *payload_ptr;
#endif
    if (payload.max_size < kFixedSerializedSize) {
      payload.reserve(kFixedSerializedSize);
    }
    // Encapsulation header: little-endian PLAIN_CDR (0x00 0x01 0x00 0x00).
    // Matches FastDDS' default DDS_CDR endianness on x86_64.
    auto* dst = reinterpret_cast<uint8_t*>(payload.data);
    dst[0] = 0x00;
    dst[1] = 0x01;
    dst[2] = 0x00;
    dst[3] = 0x00;
    std::memcpy(dst + kEncapsulationHeaderBytes, data, sizeof(T));
    payload.length = kFixedSerializedSize;
    payload.encapsulation = CDR_LE;
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
    if (payload.length < kFixedSerializedSize) {
      return false;
    }
    const auto* src = reinterpret_cast<const uint8_t*>(payload.data);
    std::memcpy(data, src + kEncapsulationHeaderBytes, sizeof(T));
    return true;
  }

#if IBMW_DDS_HAS_MODERN_FASTDDS
  uint32_t calculate_serialized_size(
      const void* const /*data*/,
      eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
    static_cast<void>(data_representation);
    return kFixedSerializedSize;
  }

  void* create_data() override { return new T(); }
  void delete_data(void* data) override { delete static_cast<T*>(data); }

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
  // Constant size -- DataSharing pre-allocates SHM chunks of exactly
  // this size, so returning a closure that recomputes anything would
  // be wasted work.
  std::function<uint32_t()> getSerializedSizeProvider(void* /*data*/) override {
    return []() -> uint32_t { return kFixedSerializedSize; };
  }

  // ========== Memory management ==========
  void* createData() override { return new T(); }
  void deleteData(void* data) override { delete static_cast<T*>(data); }

  // ========== Key support (keyless types only) ==========
  bool getKey(
      void* /*data*/,
      rtps_compat::InstanceHandle_t* /*ihandle*/,
      bool /*force_md5*/) override {
    return false;
  }
#endif
};

// ---------------------------------------------------------------------------
// IsPlainDdsType<T> trait
//
// Opt-in marker that switches GetDdsMessageTypeSupport<T>() onto the
// PlainDdsPubSubType<T> path. Default: false (everything stays on
// GenericDdsPubSubType<T>, preserving wire compatibility for every
// existing IBMW_DDS_REGISTER_CONTEXT_TYPE consumer).
//
// Use IBMW_DDS_PLAIN_TYPE(T) at namespace scope (alongside the existing
// IBMW_DDS_TYPE_NAME(T,name)) to declare a type as Plain. Compile fails
// fast if T is not trivially copyable.
template <typename T>
struct IsPlainDdsType : std::false_type {};

}  // namespace ibmw::dds

#define IBMW_DDS_PLAIN_TYPE(DdsType)                                  \
  namespace ibmw::dds {                                               \
  static_assert(std::is_trivially_copyable_v<DdsType>,                \
                "IBMW_DDS_PLAIN_TYPE: " #DdsType                      \
                " must be trivially copyable to use the LoanedMessage "\
                "/ DataSharing zero-copy path");                      \
  template <>                                                         \
  struct IsPlainDdsType<DdsType> : std::true_type {};                 \
  }
