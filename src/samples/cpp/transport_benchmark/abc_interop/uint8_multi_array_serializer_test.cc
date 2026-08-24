#include "uint8_multi_array_serializer.h"

#include <cstdint>
#include <vector>

#include "gtest/gtest.h"
#if IBMW_ABC_HAS_STD_MSGS_FASTRTPS_CPP
#include <std_msgs/msg/detail/u_int8_multi_array__rosidl_typesupport_fastrtps_cpp.hpp>
#endif

namespace {

using Message = std_msgs::msg::UInt8MultiArray;

uint16_t ExpectedEncapsulation() {
  return eprosima::fastcdr::Cdr::DEFAULT_ENDIAN ==
                 eprosima::fastcdr::Cdr::BIG_ENDIANNESS
             ? CDR_BE
             : CDR_LE;
}

void ExpectEncapsulation(const ibmw::ros2::rtps_compat::SerializedPayload_t& payload) {
  ASSERT_GE(payload.length, 4U);
  EXPECT_EQ(payload.encapsulation, ExpectedEncapsulation());
  EXPECT_EQ(payload.data[0], 0U);
  EXPECT_EQ(payload.data[1], ExpectedEncapsulation() == CDR_BE ? 0U : 1U);
  EXPECT_EQ(payload.data[2], 0U);
  EXPECT_EQ(payload.data[3], 0U);
}

void ExpectMessageEqual(const Message& lhs, const Message& rhs) {
  ASSERT_EQ(lhs.layout.dim.size(), rhs.layout.dim.size());
  for (size_t i = 0; i < lhs.layout.dim.size(); ++i) {
    EXPECT_EQ(lhs.layout.dim[i].label, rhs.layout.dim[i].label);
    EXPECT_EQ(lhs.layout.dim[i].size, rhs.layout.dim[i].size);
    EXPECT_EQ(lhs.layout.dim[i].stride, rhs.layout.dim[i].stride);
  }
  EXPECT_EQ(lhs.layout.data_offset, rhs.layout.data_offset);
  EXPECT_EQ(lhs.data, rhs.data);
}

Message RoundTrip(const Message& input, std::vector<uint8_t>* wire = nullptr) {
  ibmw::ros2::rtps_compat::SerializedPayload_t payload;
  EXPECT_TRUE(ibmw::ros2::ROS2CdrSerializer::Serialize(input, &payload));
  EXPECT_GT(payload.length, 0U);
  EXPECT_LE(payload.length,
            ibmw::ros2::ROS2CdrSerializer::EstimateSize(input));
  ExpectEncapsulation(payload);
  if (wire != nullptr) {
    wire->assign(payload.data, payload.data + payload.length);
  }

  Message output;
  EXPECT_TRUE(ibmw::ros2::ROS2CdrSerializer::Deserialize(&payload, output));
  return output;
}

TEST(UInt8MultiArraySerializer, RoundTripsEmptyLayoutAndData) {
  Message input;
  const Message output = RoundTrip(input);

  EXPECT_TRUE(output.layout.dim.empty());
  EXPECT_EQ(output.layout.data_offset, 0U);
  EXPECT_TRUE(output.data.empty());
}

TEST(UInt8MultiArraySerializer, PreservesFieldOrderAndPayloadBytes) {
  Message input;
  input.layout.dim.resize(2);
  input.layout.dim[0].label = "row";
  input.layout.dim[0].size = 2;
  input.layout.dim[0].stride = 6;
  input.layout.dim[1].label = "alignment-sensitive-label";
  input.layout.dim[1].size = 3;
  input.layout.dim[1].stride = 3;
  input.layout.data_offset = 7;
  input.data = {0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff};

  std::vector<uint8_t> wire;
  const Message output = RoundTrip(input, &wire);

  ASSERT_EQ(output.layout.dim.size(), 2U);
  EXPECT_EQ(output.layout.dim[0].label, "row");
  EXPECT_EQ(output.layout.dim[0].size, 2U);
  EXPECT_EQ(output.layout.dim[0].stride, 6U);
  EXPECT_EQ(output.layout.dim[1].label, "alignment-sensitive-label");
  EXPECT_EQ(output.layout.dim[1].size, 3U);
  EXPECT_EQ(output.layout.dim[1].stride, 3U);
  EXPECT_EQ(output.layout.data_offset, 7U);
  EXPECT_EQ(output.data, input.data);
  EXPECT_FALSE(wire.empty());
}

#if IBMW_ABC_HAS_STD_MSGS_FASTRTPS_CPP
TEST(UInt8MultiArraySerializer, MatchesGeneratedFastRtpsCallbacksExactly) {
  Message input;
  input.layout.dim.resize(2);
  input.layout.dim[0].label = "abc";
  input.layout.dim[0].size = 3;
  input.layout.dim[0].stride = 15;
  input.layout.dim[1].label = "alignment-sensitive-label";
  input.layout.dim[1].size = 5;
  input.layout.dim[1].stride = 5;
  input.layout.data_offset = 3;
  input.data = {0x00, 0x11, 0x7f, 0x80, 0xfe, 0xff, 0x42};

  ibmw::ros2::rtps_compat::SerializedPayload_t actual;
  ASSERT_TRUE(ibmw::ros2::ROS2CdrSerializer::Serialize(input, &actual));
  ExpectEncapsulation(actual);

  const uint32_t generated_size = static_cast<uint32_t>(
      4U + std_msgs::msg::typesupport_fastrtps_cpp::get_serialized_size(input, 0));
  ibmw::ros2::rtps_compat::SerializedPayload_t generated;
  generated.reserve(generated_size);
  eprosima::fastcdr::FastBuffer generated_buffer(
      reinterpret_cast<char*>(generated.data), generated.max_size);
  eprosima::fastcdr::Cdr generated_cdr(
      generated_buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
      ibmw::ros2::ROS2CdrSerializer::kCdrType);
  generated_cdr.serialize_encapsulation();
  ASSERT_TRUE(std_msgs::msg::typesupport_fastrtps_cpp::cdr_serialize(
      input, generated_cdr));
  generated.length = static_cast<uint32_t>(generated_cdr.getSerializedDataLength());
  generated.encapsulation = ExpectedEncapsulation();

  ASSERT_EQ(actual.length, generated.length);
  EXPECT_EQ(std::vector<uint8_t>(actual.data, actual.data + actual.length),
            std::vector<uint8_t>(generated.data,
                                 generated.data + generated.length));

  Message generated_from_actual;
  eprosima::fastcdr::FastBuffer actual_buffer(
      reinterpret_cast<char*>(actual.data), actual.length);
  eprosima::fastcdr::Cdr actual_cdr(
      actual_buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
      ibmw::ros2::ROS2CdrSerializer::kCdrType);
  actual_cdr.read_encapsulation();
  ASSERT_TRUE(std_msgs::msg::typesupport_fastrtps_cpp::cdr_deserialize(
      actual_cdr, generated_from_actual));
  ExpectMessageEqual(input, generated_from_actual);

  Message custom_from_generated;
  ASSERT_TRUE(ibmw::ros2::ROS2CdrSerializer::Deserialize(
      &generated, custom_from_generated));
  ExpectMessageEqual(input, custom_from_generated);
}
#endif

}  // namespace
