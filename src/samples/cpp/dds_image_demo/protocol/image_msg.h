// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Hand-written DDS type for the dds_image_demo example.
// This demonstrates how to define a DDS type with enums, fixed-size arrays,
// and FastCDR v1 serialization without requiring fastddsgen code generation.

#pragma once

#include <array>
#include <cstdint>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

// v2-to-v1 compat: provides CdrSizeCalculator for size estimation
#include "dds_types/util/fastcdr_v1_compat.h"

namespace ibmw::examples::dds_image_demo {

/**
 * @brief Image encoding types.
 */
enum class ImageEncoding : uint32_t {
  RGB8 = 0,   // 8-bit RGB
  RGBA8 = 1,  // 8-bit RGBA
  GRAY8 = 2,  // 8-bit grayscale
  GRAY16 = 3  // 16-bit grayscale
};

/**
 * @brief Fixed-size image buffer size (1 MB).
 * Supports up to 640x480 RGB8 (921,600 bytes) or similar.
 */
static constexpr size_t kImageBufferSize = 1048576;

/**
 * @brief Image message type for DDS zero-copy communication.
 *
 * Fields:
 *   frame_id   - Frame sequence number
 *   timestamp  - Timestamp in microseconds since epoch
 *   width      - Image width in pixels
 *   height     - Image height in pixels
 *   encoding   - Image encoding type
 *   data       - Fixed-size image data buffer (1 MB)
 *   data_size  - Actual data size in bytes
 */
struct ImageMsg {
  uint64_t frame_id = 0;
  int64_t timestamp = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  ImageEncoding encoding = ImageEncoding::RGB8;
  std::array<uint8_t, kImageBufferSize> data{};
  uint32_t data_size = 0;

  // FastCDR v1 requires serialize/deserialize member functions
  // (called by Cdr::operator<< and Cdr::operator>>)
  void serialize(eprosima::fastcdr::Cdr& cdr) const {
    cdr << frame_id << timestamp << width << height
        << static_cast<uint32_t>(encoding) << data << data_size;
  }

  void deserialize(eprosima::fastcdr::Cdr& cdr) {
    cdr >> frame_id >> timestamp >> width >> height;
    uint32_t enc_val;
    cdr >> enc_val;
    encoding = static_cast<ImageEncoding>(enc_val);
    cdr >> data >> data_size;
  }
};

}  // namespace ibmw::examples::dds_image_demo

// Register DDS type name
#include "dds_types/util/dds_type_support.h"
IBMW_DDS_TYPE_NAME(ibmw::examples::dds_image_demo::ImageMsg,
                   "ibmw::examples::dds_image_demo::ImageMsg")

// FastCDR v1 serialize/deserialize specializations
namespace eprosima {
namespace fastcdr {

// Size calculation for the compat CdrSizeCalculator
template <>
inline size_t calculate_serialized_size(
    CdrSizeCalculator& calculator,
    const ibmw::examples::dds_image_demo::ImageMsg& data,
    size_t& current_alignment) {
  size_t total = 0;
  total += calculator.calculate_member_serialized_size(
      MemberId(0), data.frame_id, current_alignment);
  total += calculator.calculate_member_serialized_size(
      MemberId(1), data.timestamp, current_alignment);
  total += calculator.calculate_member_serialized_size(
      MemberId(2), data.width, current_alignment);
  total += calculator.calculate_member_serialized_size(
      MemberId(3), data.height, current_alignment);
  total += calculator.calculate_member_serialized_size(
      MemberId(4), static_cast<uint32_t>(data.encoding), current_alignment);
  total += calculator.calculate_member_serialized_size(
      MemberId(5), data.data, current_alignment);
  total += calculator.calculate_member_serialized_size(
      MemberId(6), data.data_size, current_alignment);
  return total;
}

// Serialize: use operator<< for each field in order (PLAIN_CDR format)
template <>
inline void serialize(
    Cdr& scdr,
    const ibmw::examples::dds_image_demo::ImageMsg& data) {
  scdr << data.frame_id
       << data.timestamp
       << data.width
       << data.height
       << static_cast<uint32_t>(data.encoding)
       << data.data
       << data.data_size;
}

// Deserialize: use operator>> for each field in order
template <>
inline void deserialize(
    Cdr& cdr,
    ibmw::examples::dds_image_demo::ImageMsg& data) {
  cdr >> data.frame_id
      >> data.timestamp
      >> data.width
      >> data.height;

  // Enum: read as uint32_t then cast
  uint32_t enc_val;
  cdr >> enc_val;
  data.encoding = static_cast<ibmw::examples::dds_image_demo::ImageEncoding>(enc_val);

  cdr >> data.data
      >> data.data_size;
}

}  // namespace fastcdr
}  // namespace eprosima
