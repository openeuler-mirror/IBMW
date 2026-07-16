// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Hand-written DDS type for the dds_native_demo example.
// This demonstrates how to define a DDS type and use FastCDR v1 serialization
// without requiring fastddsgen code generation.

#pragma once

#include <cstdint>
#include <string>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

// v2-to-v1 compat: provides CdrSizeCalculator for size estimation
#include "dds_types/util/fastcdr_v1_compat.h"
#include "dds_types/util/dds_type_support.h"

namespace ibmw::examples::dds_native_demo {

/**
 * @brief Simple DDS message type.
 *
 * Fields:
 *   uint64_t id        - sequence number
 *   std::string message - text payload
 *   int64_t timestamp   - milliseconds since epoch
 */
struct HelloMsg {
  uint64_t id = 0;
  std::string message;
  int64_t timestamp = 0;

  HelloMsg() = default;
  HelloMsg(uint64_t id_, std::string msg_, int64_t ts_)
      : id(id_), message(std::move(msg_)), timestamp(ts_) {}

  // FastCDR v1 requires serialize/deserialize member functions
  // (called by Cdr::operator<< and Cdr::operator>>)
  void serialize(eprosima::fastcdr::Cdr& cdr) const {
    cdr << id << message << timestamp;
  }

  void deserialize(eprosima::fastcdr::Cdr& cdr) {
    cdr >> id >> message >> timestamp;
  }
};

}  // namespace ibmw::examples::dds_native_demo

// Provide FastCDR v1 operator<< / operator>> for serialization.
// These are the v1-compatible equivalents of the v2 free-function
// serialize/deserialize specializations that fastddsgen would generate.
namespace eprosima {
namespace fastcdr {

// Size calculation for the compat CdrSizeCalculator
template <>
inline size_t calculate_serialized_size(
    CdrSizeCalculator& calculator,
    const ibmw::examples::dds_native_demo::HelloMsg& data,
    size_t& current_alignment) {
  size_t total = 0;
  // id: uint64_t (8 bytes, 8-byte aligned)
  total += calculator.calculate_member_serialized_size(
      MemberId(0), data.id, current_alignment);
  // message: string (4-byte length + chars + null)
  total += calculator.calculate_member_serialized_size(
      MemberId(1), data.message, current_alignment);
  // timestamp: int64_t (8 bytes, 8-byte aligned)
  total += calculator.calculate_member_serialized_size(
      MemberId(2), data.timestamp, current_alignment);
  return total;
}

// Serialize: use operator<< for each field in order (PLAIN_CDR format)
template <>
inline void serialize(
    Cdr& scdr,
    const ibmw::examples::dds_native_demo::HelloMsg& data) {
  scdr << data.id
       << data.message
       << data.timestamp;
}

// Deserialize: use operator>> for each field in order
template <>
inline void deserialize(
    Cdr& cdr,
    ibmw::examples::dds_native_demo::HelloMsg& data) {
  cdr >> data.id
      >> data.message
      >> data.timestamp;
}

}  // namespace fastcdr
}  // namespace eprosima

// Register DDS type name for the HelloMsg type
IBMW_DDS_TYPE_NAME(ibmw::examples::dds_native_demo::HelloMsg,
                   "ibmw::examples::dds_native_demo::HelloMsg")
