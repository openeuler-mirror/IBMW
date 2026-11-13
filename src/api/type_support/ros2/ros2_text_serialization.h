// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once

#include <algorithm>
#include <cstddef>
#include <stdexcept>

#include <rosidl_typesupport_introspection_cpp/field_types.hpp>
#include <rosidl_typesupport_introspection_cpp/message_introspection.hpp>
#include <rosidl_typesupport_introspection_cpp/message_type_support_decl.hpp>

#include "type_support/text_serialization.h"

namespace ibmw::ros2::text_detail {
using ibmw::util::text::Json;
using rosidl_typesupport_introspection_cpp::MessageMember;
using rosidl_typesupport_introspection_cpp::MessageMembers;

// Upper bound on the storage resizable arrays may claim while one message is
// read. A short JSON element such as `{}` can stand for a large nested
// message, so the input size alone does not bound the allocation.
inline constexpr size_t kMaxTextArrayBytes = size_t{256} << 20;

template <typename T>
const MessageMembers& Members() {
  static const auto* members = static_cast<const MessageMembers*>(
      rosidl_typesupport_introspection_cpp::get_message_type_support_handle<T>()->data);
  return *members;
}

inline void ReadMessage(const Json& value, void* message, const MessageMembers& members, size_t depth,
                        size_t& array_budget);
inline Json WriteMessage(const void* message, const MessageMembers& members, size_t depth = 0);

template <typename Callback> void Scalar(uint8_t type, Callback&& callback) {
  using namespace rosidl_typesupport_introspection_cpp;  // ROS_TYPE_* constants
  switch (type) {
    case ROS_TYPE_FLOAT: callback(static_cast<float*>(nullptr)); break;
    case ROS_TYPE_DOUBLE: callback(static_cast<double*>(nullptr)); break;
    case ROS_TYPE_LONG_DOUBLE: callback(static_cast<long double*>(nullptr)); break;
    case ROS_TYPE_CHAR:
    case ROS_TYPE_OCTET:
    case ROS_TYPE_UINT8: callback(static_cast<uint8_t*>(nullptr)); break;
    case ROS_TYPE_WCHAR: callback(static_cast<char16_t*>(nullptr)); break;
    case ROS_TYPE_BOOLEAN: callback(static_cast<bool*>(nullptr)); break;
    case ROS_TYPE_INT8: callback(static_cast<int8_t*>(nullptr)); break;
    case ROS_TYPE_UINT16: callback(static_cast<uint16_t*>(nullptr)); break;
    case ROS_TYPE_INT16: callback(static_cast<int16_t*>(nullptr)); break;
    case ROS_TYPE_UINT32: callback(static_cast<uint32_t*>(nullptr)); break;
    case ROS_TYPE_INT32: callback(static_cast<int32_t*>(nullptr)); break;
    case ROS_TYPE_UINT64: callback(static_cast<uint64_t*>(nullptr)); break;
    case ROS_TYPE_INT64: callback(static_cast<int64_t*>(nullptr)); break;
    case ROS_TYPE_STRING: callback(static_cast<std::string*>(nullptr)); break;
    default: throw std::invalid_argument("Unsupported ROS 2 text field type.");
  }
}

inline size_t ElementSize(const MessageMember& f) {
  if (f.type_id_ == rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE)
    return static_cast<const MessageMembers*>(f.members_->data)->size_of_;
  size_t size = 0;
  Scalar(f.type_id_, [&](auto* tag) { size = sizeof(*tag); });
  return size;
}

inline void ReadMessage(const Json& value, void* message, const MessageMembers& members, size_t depth,
                        size_t& array_budget) {
  using rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE;
  if (depth > 128 || !value.is_object()) throw std::invalid_argument("Expected a message object (maximum depth 128).");
  for (auto it = value.begin(); it != value.end(); ++it) {
    const MessageMember* field = nullptr;
    for (uint32_t i = 0; i < members.member_count_; ++i)
      if (it.key() == members.members_[i].name_) {
        field = &members.members_[i];
        break;
      }
    if (!field) throw std::invalid_argument("Unknown message field: " + it.key());
    const auto& f = *field;
    void* storage = static_cast<char*>(message) + f.offset_;
    const auto& input = it.value();
    size_t count = 1;
    if (f.is_array_) {
      if (!input.is_array()) throw std::invalid_argument("Expected an array: " + it.key());
      count = input.size();
      if (f.array_size_ && (f.is_upper_bound_ ? count > f.array_size_ : count != f.array_size_))
        throw std::invalid_argument("Invalid array length: " + it.key());
      if (!f.array_size_ || f.is_upper_bound_) {
        if (!f.resize_function) throw std::invalid_argument("Array cannot be resized: " + it.key());
        const size_t element_size = std::max<size_t>(ElementSize(f), 1);
        if (count > array_budget / element_size)
          throw std::length_error("Arrays exceed the decoded size limit: " + it.key());
        array_budget -= count * element_size;
        f.resize_function(storage, count);
      }
    }
    for (size_t index = 0; index < count; ++index) {
      const auto& item = f.is_array_ ? input[index] : input;
      if (f.type_id_ == ROS_TYPE_MESSAGE) {
        void* nested = f.is_array_ ? f.get_function(storage, index) : storage;
        ReadMessage(item, nested, *static_cast<const MessageMembers*>(f.members_->data), depth + 1,
                    array_budget);
      } else {
        Scalar(f.type_id_, [&](auto* tag) {
          using T = std::remove_pointer_t<decltype(tag)>;
          T converted{};
          ibmw::util::text::Read(item, converted);
          if constexpr (std::is_same_v<T, std::string>) {
            if (f.string_upper_bound_ && converted.size() > f.string_upper_bound_)
              throw std::out_of_range("String exceeds its bound: " + it.key());
          }
          if (!f.is_array_) *static_cast<T*>(storage) = std::move(converted);
          else if (f.assign_function) f.assign_function(storage, index, &converted);
          else *static_cast<T*>(f.get_function(storage, index)) = std::move(converted);
        });
      }
    }
  }
}

inline void ReadMessage(const Json& value, void* message, const MessageMembers& members) {
  size_t array_budget = kMaxTextArrayBytes;
  ReadMessage(value, message, members, 0, array_budget);
}

inline Json WriteMessage(const void* message, const MessageMembers& members, size_t depth) {
  using rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE;
  if (depth > 128) throw std::invalid_argument("Message nesting exceeds 128.");
  auto result = Json::object();
  for (uint32_t i = 0; i < members.member_count_; ++i) {
    const auto& f = members.members_[i];
    const void* storage = static_cast<const char*>(message) + f.offset_;
    const size_t count = f.is_array_ ? f.size_function(storage) : 1;
    Json field = f.is_array_ ? Json::array() : Json{};
    for (size_t index = 0; index < count; ++index) {
      Json item;
      if (f.type_id_ == ROS_TYPE_MESSAGE) {
        const void* nested = f.is_array_ ? f.get_const_function(storage, index) : storage;
        item = WriteMessage(nested, *static_cast<const MessageMembers*>(f.members_->data), depth + 1);
      } else {
        Scalar(f.type_id_, [&](auto* tag) {
          using T = std::remove_pointer_t<decltype(tag)>;
          T value{};
          if (!f.is_array_) value = *static_cast<const T*>(storage);
          else if (f.fetch_function) f.fetch_function(storage, index, &value);
          else value = *static_cast<const T*>(f.get_const_function(storage, index));
          item = ibmw::util::text::Write(value);
        });
      }
      if (f.is_array_) field.push_back(std::move(item));
      else field = std::move(item);
    }
    result[f.name_] = std::move(field);
  }
  return result;
}
}  // namespace ibmw::ros2::text_detail
