// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "type_support/type_support_pkg_main.h"

#include <cstring>
#include <string_view>

#include "Header.hpp"
#include "HeaderCdrAux.ipp"
#include "HeaderSchema.hpp"
#include "cpp/util/buffer.h"
#include "dds_types/util/dds_type_support.h"
#include "example_ros2/msg/ros_test_msg.hpp"
#include "nlohmann/json.hpp"
#include "ros2_types/util/ros2_schema_support.h"
#include "ros2_types/util/ros2_type_support_factory.h"

using Header = ibmw::protocols::common::Header;

IBMW_DDS_TYPE_NAME(Header, "ibmw::protocols::common::Header")

namespace {

const auto* HeaderCdrSupport() {
  return ibmw::dds::GetDdsMessageTypeSupport<Header>(
      "ibmw::protocols::common::Header");
}

bool IsSerialization(ibmw_string_view_t value, std::string_view expected) {
  return value.str != nullptr &&
         std::string_view(value.str, value.len) == expected;
}

const ibmw_string_view_t kSerializations[] = {
    ibmw::util::ToIbmwStringView("cdr"),
    ibmw::util::ToIbmwStringView("json")};

const ibmw_type_support_base_t* HeaderTranscodingSupport() {
  static const std::string kTypeName =
      "dds:ibmw::protocols::common::Header";
  static const ibmw_type_support_base_t kSupport{
      .type_name = [](void*) {
        return ibmw::util::ToIbmwStringView(kTypeName);
      },
      .create = [](void* context) {
        return HeaderCdrSupport()->create(context);
      },
      .destroy = [](void* context, void* message) {
        HeaderCdrSupport()->destroy(context, message);
      },
      .copy = [](void* context, const void* from, void* to) {
        HeaderCdrSupport()->copy(context, from, to);
      },
      .move = [](void* context, void* from, void* to) {
        HeaderCdrSupport()->move(context, from, to);
      },
      .serialize = [](void* context, ibmw_string_view_t serialization,
                      const void* message,
                      const ibmw_buffer_array_allocator_t* allocator,
                      ibmw_buffer_array_t* buffers) {
        if (IsSerialization(serialization, "cdr")) {
          return HeaderCdrSupport()->serialize(
              context, serialization, message, allocator, buffers);
        }
        if (!IsSerialization(serialization, "json") || message == nullptr ||
            allocator == nullptr || allocator->allocate == nullptr ||
            buffers == nullptr) {
          return false;
        }
        try {
          const auto& value = *static_cast<const Header*>(message);
          const auto text = nlohmann::json{
              {"time_stamp", value.time_stamp()},
              {"frame_id", value.frame_id()},
              {"sequence_num", value.sequence_num()}}.dump();
          auto output = allocator->allocate(
              allocator->ctx, buffers, text.size());
          if (output.data == nullptr || output.len < text.size() ||
              buffers->len == 0) {
            return false;
          }
          std::memcpy(output.data, text.data(), text.size());
          buffers->data[buffers->len - 1].len = text.size();
          return true;
        } catch (...) {
          return false;
        }
      },
      .deserialize = [](void* context, ibmw_string_view_t serialization,
                        ibmw_buffer_array_view_t buffers, void* message) {
        if (IsSerialization(serialization, "cdr")) {
          return HeaderCdrSupport()->deserialize(
              context, serialization, buffers, message);
        }
        if (!IsSerialization(serialization, "json") || message == nullptr ||
            buffers.data == nullptr || buffers.len == 0) {
          return false;
        }
        try {
          std::string text;
          for (size_t index = 0; index < buffers.len; ++index) {
            text.append(
                static_cast<const char*>(buffers.data[index].data),
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
        return kSerializations;
      },
      .custom_type_support_ptr = [](void* context) {
        return HeaderCdrSupport()->custom_type_support_ptr(context);
      },
      .ctx = nullptr};
  return &kSupport;
}

const ibmw_schema_support_base_t* HeaderJsonSchema() {
  static const ibmw::util::SchemaSupport kSchema(
      ibmw::util::SchemaDescriptor{
          .type_name = "dds:ibmw::protocols::common::Header",
          .serialization_type = "json",
          .schema_name = "ibmw::protocols::common::Header",
          .schema_encoding = "jsonschema",
          .schema_data = R"({"type":"object","required":["time_stamp","frame_id","sequence_num"],"properties":{"time_stamp":{"type":"integer"},"frame_id":{"type":"string"},"sequence_num":{"type":"integer"}}})"});
  return kSchema.NativeHandle();
}

const ibmw_type_support_base_t* type_support_array[]{
    HeaderTranscodingSupport(),
    ibmw::GetRos2MessageTypeSupport<example_ros2::msg::RosTestMsg>()};
const ibmw_schema_support_base_t* schema_support_array[]{
    ibmw::dds::GetDdsMessageSchemaSupport<Header>(
        "ibmw::protocols::common::Header", ibmw::dds::schema::kHeaderIdl,
        "ibmw::protocols::common::Header"),
    HeaderJsonSchema(),
    ibmw::ros2::GetRos2MessageSchemaSupport<
        example_ros2::msg::RosTestMsg>()};

const ibmw_schema_support_package_v1_t schema_support_package{
    .abi_version = IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_schema_support_package_v1_t),
    .entry_count = std::size(schema_support_array),
    .entries = schema_support_array,
    .reserved = {}};

const ibmw_type_support_package_v1_t type_support_package{
    .abi_version = IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V1,
    .struct_size = sizeof(ibmw_type_support_package_v1_t),
    .entry_count = std::size(type_support_array),
    .entries = type_support_array,
    .schema_support = &schema_support_package,
    .reserved = {}};

}  // namespace

extern "C" {
const ibmw_type_support_package_v1_t* IbmwDynlibGetTypeSupportPackageV1() {
  return &type_support_package;
}
}
