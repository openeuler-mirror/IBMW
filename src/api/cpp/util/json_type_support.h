// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "c/util/type_support_base.h"
#include "cpp/util/schema_support.h"
#include "cpp/util/string.h"

namespace ibmw::json {

template <typename JsonType>
const ibmw_schema_support_base_t* GetJsonMessageSchemaSupport() {
  static_assert(
      requires { JsonType::kJsonSchema; },
      "JSON Schema is missing. Generate this type with ibmw-gen or provide "
      "a static constexpr kJsonSchema member.");

  static const ibmw::util::SchemaSupport kSchema(
      ibmw::util::SchemaDescriptor{
          .type_name = "json:" + std::string(JsonType::kTypeName),
          .serialization_type = "json",
          .schema_name = JsonType::kTypeName,
          .schema_encoding = "jsonschema",
          .schema_data = JsonType::kJsonSchema,
          .schema_hash = {}});
  return kSchema.NativeHandle();
}

/**
 * @brief Get TypeSupport for JSON-serializable structs (management extension types)
 *
 * This creates a TypeSupport that provides:
 * - JSON serialization/deserialization via nlohmann::json ADL (to_json/from_json)
 * - The "json" serialization type
 *
 * In ibmw, management extension interfaces use nlohmann::json + hand-written C++ structs
 * instead of protobuf code generation. This utility provides the type support bridge
 * between those structs and the ibmw RPC framework.
 *
 * Requirements for JsonType:
 * - Default-constructible
 * - Copy-assignable and move-assignable
 * - Has ADL to_json(json&, const JsonType&) and from_json(const json&, JsonType&)
 *   (e.g., via NLOHMANN_DEFINE_TYPE_INTRUSIVE or free functions)
 *
 * @tparam JsonType The C++ struct type with nlohmann::json serialization support
 * @tparam TypeNameTag A compile-time string tag used as the type name
 * @return Pointer to the TypeSupport structure
 */
template <typename JsonType>
const ibmw_type_support_base_t* GetJsonMessageTypeSupport() {
  // Type name derived from the struct
  static const std::string kMsgTypeName = "json:" + std::string(JsonType::kTypeName);

  // Supported serialization types
  static const std::string kJsonType = "json";
  static const ibmw_string_view_t kSerializationTypes[] = {
      ibmw::util::ToIbmwStringView(kJsonType),
  };
  static constexpr size_t kNumSerializationTypes =
      sizeof(kSerializationTypes) / sizeof(kSerializationTypes[0]);

  // Static TypeSupport with JSON serialization support
  static const ibmw_type_support_base_t kTs{
      .type_name = [](void* ctx) -> ibmw_string_view_t {
        return ibmw::util::ToIbmwStringView(kMsgTypeName);
      },
      .create = [](void* ctx) -> void* {
        return new JsonType();
      },
      .destroy = [](void* ctx, void* msg) {
        delete static_cast<JsonType*>(msg);
      },
      .copy = [](void* ctx, const void* from, void* to) {
        *static_cast<JsonType*>(to) = *static_cast<const JsonType*>(from);
      },
      .move = [](void* ctx, void* from, void* to) {
        *static_cast<JsonType*>(to) = std::move(*static_cast<JsonType*>(from));
      },
      .serialize = [](void* ctx,
                      ibmw_string_view_t serialization_type,
                      const void* msg,
                      const ibmw_buffer_array_allocator_t* allocator,
                      ibmw_buffer_array_t* buffer_array) -> bool {
        std::string_view ser_type(serialization_type.str, serialization_type.len);

        if (ser_type == "json") {
          try {
            nlohmann::json j;
            to_json(j, *static_cast<const JsonType*>(msg));
            std::string json_str = j.dump();

            // Allocate buffer via the framework allocator
            ibmw_buffer_t buf = allocator->allocate(
                allocator->ctx, buffer_array, json_str.size());
            if (buf.data == nullptr || buf.len < json_str.size()) {
              return false;
            }

            std::memcpy(buf.data, json_str.data(), json_str.size());

            // Update the last buffer with actual size
            if (buffer_array->len > 0) {
              buffer_array->data[buffer_array->len - 1].len = json_str.size();
            }

            return true;
          } catch (...) {
            return false;
          }
        }

        // Unknown serialization type
        return false;
      },
      .deserialize = [](void* ctx,
                        ibmw_string_view_t serialization_type,
                        ibmw_buffer_array_view_t buffer_array_view,
                        void* msg) -> bool {
        std::string_view ser_type(serialization_type.str, serialization_type.len);

        if (ser_type == "json") {
          try {
            // Collect all buffers into a single string
            std::string json_str;
            for (size_t i = 0; i < buffer_array_view.len; ++i) {
              json_str.append(
                  static_cast<const char*>(buffer_array_view.data[i].data),
                  buffer_array_view.data[i].len);
            }

            if (json_str.empty()) return false;

            nlohmann::json j = nlohmann::json::parse(json_str);
            from_json(j, *static_cast<JsonType*>(msg));

            return true;
          } catch (...) {
            return false;
          }
        }

        // Unknown serialization type
        return false;
      },
      .serialization_types_supported_num = [](void* ctx) -> size_t {
        return kNumSerializationTypes;
      },
      .serialization_types_supported_list = [](void* ctx) -> const ibmw_string_view_t* {
        return kSerializationTypes;
      },
      .custom_type_support_ptr = [](void* ctx) -> const void* {
        return nullptr;  // No custom type support for JSON types
      },
      .msg_min_size = sizeof(JsonType),
      .ctx = nullptr};

  return &kTs;
}

}  // namespace ibmw::json
