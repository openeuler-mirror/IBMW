// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "cpp/util/json_type_support.h"
#include "cpp/util/buffer.h"
#include "type_support/type_support_pkg_main.h"

namespace {
struct Message {
  static constexpr const char* kTypeName = "EchoFixture";
  std::string value;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE(Message, value)
};

const ibmw_type_support_base_t* TypeSupport() {
  static const auto support = [] {
    auto result = *ibmw::json::GetJsonMessageTypeSupport<Message>();
    result.type_name = [](void*) { return ibmw::util::ToIbmwStringView("test:Bytes"); };
    result.serialization_types_supported_num = [](void*) -> size_t { return 2; };
    result.serialization_types_supported_list = [](void*) {
      static const ibmw_string_view_t formats[] = {
          ibmw::util::ToIbmwStringView("test_binary"), ibmw::util::ToIbmwStringView("json")};
      return formats;
    };
    result.deserialize = [](void*, ibmw_string_view_t format, ibmw_buffer_array_view_t buffers, void* object) {
      if (ibmw::util::ToStdStringView(format) != "test_binary") return false;
      const auto bytes = ibmw::util::BufferArrayView(buffers).JoinToString();
      if (bytes.empty() || bytes.front() != '\x01') return false;
      static_cast<Message*>(object)->value = bytes.substr(1);
      return true;
    };
    return result;
  }();
  return &support;
}

// Lightweight decoder contracts keep the formatter tests runnable without ROS
// or DDS libraries. The marker byte lets the tests detect decoder selection.
constexpr struct {
  const char* type;
  const char* format;
  char marker;
} kWireDecoders[] = {
    {"ros2:fixture_msgs/msg/Text", "ros2", '\x01'},
    {"ros2:fixture_msgs/srv/Query_Request", "ros2", '\x01'},
    {"ros2:fixture_msgs/action/Move_FeedbackMessage", "ros2", '\x01'},
    {"dds:fixture_msgs::msg::dds_::Text_", "cdr", '\x02'},
    {"ros2:fixture_msgs/msg/Unsupported", "ros2", '\x01'},
    {"dds:fixture_msgs::msg::dds_::Unsupported_", "test_binary", '\x01'},
    {"ros2:fixture_msgs/msg/WrongFormat", "test_binary", '\x01'}};

template <size_t Index>
const ibmw_type_support_base_t* WireDecoder() {
  static const auto support = [] {
    auto result = *TypeSupport();
    result.type_name = [](void*) { return ibmw::util::ToIbmwStringView(kWireDecoders[Index].type); };
    result.serialization_types_supported_list = [](void*) {
      static const ibmw_string_view_t formats[] = {
          ibmw::util::ToIbmwStringView(kWireDecoders[Index].format),
          ibmw::util::ToIbmwStringView("json")};
      return formats;
    };
    result.deserialize = [](void*, ibmw_string_view_t format, ibmw_buffer_array_view_t buffers, void* object) {
      if (ibmw::util::ToStdStringView(format) != kWireDecoders[Index].format) return false;
      const auto bytes = ibmw::util::BufferArrayView(buffers).JoinToString();
      if (bytes.empty() || bytes.front() != kWireDecoders[Index].marker) return false;
      static_cast<Message*>(object)->value = bytes.substr(1);
      return true;
    };
    return result;
  }();
  return &support;
}
}  // namespace

extern "C" const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  static const ibmw_type_support_base_t* entries[] = {
      TypeSupport(), WireDecoder<0>(), WireDecoder<1>(), WireDecoder<2>(),
#ifdef ECHO_NATIVE_DECODER_FIXTURE
      WireDecoder<3>(), WireDecoder<5>(),
#endif
      WireDecoder<4>(), WireDecoder<6>()};
  static const ibmw_type_support_package_v1_t package{
      .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V2,
      .struct_size = sizeof(ibmw_type_support_package_v1_t),
      .entry_count = sizeof(entries) / sizeof(entries[0]), .entries = entries};
  return &package;
}
