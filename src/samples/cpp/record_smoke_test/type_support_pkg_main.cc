// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_support/type_support_pkg_main.h"

#include <cstring>
#include <string_view>

#include "cpp/util/json_type_support.h"
#include "cpp/util/buffer.h"
#include "dds_types/util/dds_type_support.h"
#include "example_ros2/msg/ros_test_msg.hpp"
#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "HeaderSchema.hpp"
#include "record_smoke_message.h"
#include "ros2_types/util/ros2_schema_support.h"
#include "ros2_types/util/ros2_type_support_factory.h"
#include "rosgraph_msgs/msg/clock.hpp"

using JsonSmokeMessage =
    ibmw::examples::cpp::record_smoke_test::JsonSmokeMessage;
using Header = ibmw::protocols::common::Header;

namespace {

const ibmw_type_support_base_t* HeaderCdrTypeSupport() {
  return ibmw::dds::GetDdsMessageTypeSupport<Header>(
      "ibmw::protocols::common::Header");
}

inline bool IsSerialization(
    ibmw_string_view_t value,
    std::string_view expected) {
  return value.str != nullptr &&
         std::string_view(value.str, value.len) == expected;
}

const ibmw_string_view_t kHeaderSerializationTypes[] = {
    ibmw::util::ToIbmwStringView("cdr"),
    ibmw::util::ToIbmwStringView("json")};

const ibmw_type_support_base_t* HeaderTranscodingTypeSupport() {
  static const std::string kTypeName =
      "dds:ibmw::protocols::common::Header";
  static const ibmw_type_support_base_t kTypeSupport{
      .type_name = [](void*) {
        return ibmw::util::ToIbmwStringView(kTypeName);
      },
      .create = [](void* ctx) { return HeaderCdrTypeSupport()->create(ctx); },
      .destroy = [](void* ctx, void* message) {
        HeaderCdrTypeSupport()->destroy(ctx, message);
      },
      .copy = [](void* ctx, const void* from, void* to) {
        HeaderCdrTypeSupport()->copy(ctx, from, to);
      },
      .move = [](void* ctx, void* from, void* to) {
        HeaderCdrTypeSupport()->move(ctx, from, to);
      },
      .serialize = [](void* ctx,
                      ibmw_string_view_t serialization_type,
                      const void* message,
                      const ibmw_buffer_array_allocator_t* allocator,
                      ibmw_buffer_array_t* buffers) {
        if (IsSerialization(serialization_type, "cdr")) {
          return HeaderCdrTypeSupport()->serialize(
              ctx, serialization_type, message, allocator, buffers);
        }
        if (!IsSerialization(serialization_type, "json") ||
            message == nullptr || allocator == nullptr ||
            allocator->allocate == nullptr || buffers == nullptr) {
          return false;
        }
        try {
          const auto& value = *static_cast<const Header*>(message);
          const nlohmann::json json{
              {"time_stamp", value.time_stamp()},
              {"frame_id", value.frame_id()},
              {"sequence_num", value.sequence_num()}};
          const auto text = json.dump();
          auto buffer = allocator->allocate(
              allocator->ctx, buffers, text.size());
          if (buffer.data == nullptr || buffer.len < text.size() ||
              buffers->len == 0) {
            return false;
          }
          std::memcpy(buffer.data, text.data(), text.size());
          buffers->data[buffers->len - 1].len = text.size();
          return true;
        } catch (...) {
          return false;
        }
      },
      .deserialize = [](void* ctx,
                        ibmw_string_view_t serialization_type,
                        ibmw_buffer_array_view_t buffers,
                        void* message) {
        if (IsSerialization(serialization_type, "cdr")) {
          return HeaderCdrTypeSupport()->deserialize(
              ctx, serialization_type, buffers, message);
        }
        if (!IsSerialization(serialization_type, "json") ||
            message == nullptr || buffers.data == nullptr || buffers.len == 0) {
          return false;
        }
        try {
          std::string text;
          for (size_t index = 0; index < buffers.len; ++index) {
            text.append(static_cast<const char*>(buffers.data[index].data),
                        buffers.data[index].len);
          }
          const auto json = nlohmann::json::parse(text);
          auto& value = *static_cast<Header*>(message);
          value.time_stamp(json.at("time_stamp").get<uint64_t>());
          value.frame_id(json.at("frame_id").get<std::string>());
          value.sequence_num(json.at("sequence_num").get<uint64_t>());
          return true;
        } catch (...) {
          return false;
        }
      },
      .serialization_types_supported_num = [](void*) { return size_t{2}; },
      .serialization_types_supported_list = [](void*) {
        return kHeaderSerializationTypes;
      },
      .custom_type_support_ptr = [](void* ctx) {
        return HeaderCdrTypeSupport()->custom_type_support_ptr(ctx);
      },
      .ctx = nullptr};
  return &kTypeSupport;
}

const ibmw_schema_support_base_t* HeaderJsonSchemaSupport() {
  static const ibmw::util::SchemaSupport schema(
      ibmw::util::SchemaDescriptor{
          .type_name = "dds:ibmw::protocols::common::Header",
          .serialization_type = "json",
          .schema_name = "ibmw::protocols::common::Header",
          .schema_encoding = "jsonschema",
          .schema_data = R"({"type":"object","required":["time_stamp","frame_id","sequence_num"],"properties":{"time_stamp":{"type":"integer"},"frame_id":{"type":"string"},"sequence_num":{"type":"integer"}}})"});
  return schema.NativeHandle();
}

}  // namespace

static const ibmw_type_support_base_t* type_support_array[]{
    HeaderTranscodingTypeSupport(),
    ibmw::GetRos2MessageTypeSupport<example_ros2::msg::RosTestMsg>(),
    ibmw::GetRos2MessageTypeSupport<rosgraph_msgs::msg::Clock>(),
    ibmw::json::GetJsonMessageTypeSupport<JsonSmokeMessage>()};

static const ibmw_schema_support_base_t* schema_support_array[]{
    ibmw::dds::GetDdsMessageSchemaSupport<Header>(
        "ibmw::protocols::common::Header", ibmw::dds::schema::kHeaderIdl,
        "ibmw::protocols::common::Header"),
    HeaderJsonSchemaSupport(),
    ibmw::ros2::GetRos2MessageSchemaSupport<example_ros2::msg::RosTestMsg>(),
    ibmw::ros2::GetRos2MessageSchemaSupport<rosgraph_msgs::msg::Clock>(),
    ibmw::json::GetJsonMessageSchemaSupport<JsonSmokeMessage>()};

static const ibmw_schema_support_package_v1_t schema_support_package{
    .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_schema_support_package_v1_t),
    .entry_count = sizeof(schema_support_array) / sizeof(schema_support_array[0]),
    .entries = schema_support_array,
    .reserved = {}};

static const ibmw_type_support_package_v1_t type_support_package{
    .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_type_support_package_v1_t),
    .entry_count = sizeof(type_support_array) / sizeof(type_support_array[0]),
    .entries = type_support_array,
    .schema_support = &schema_support_package,
    .reserved = {}};

extern "C" {
const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  return &type_support_package;
}

}  // extern "C"
