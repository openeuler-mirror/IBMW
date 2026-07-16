// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <functional>
#include <vector>

#include <fastdds/dds/topic/TopicDataType.hpp>

// Fix conflict: ROS Humble's FastRTPS TypesBase.h defines MEMBER_ID_INVALID as
// a macro (#define MEMBER_ID_INVALID 0X0FFFFFFF), which clashes with FastCDR v2's
// MemberId.hpp that uses it as a static const variable name. Undefine before
// FastCDR v2 headers are pulled in via ros2_type_support.h -> ros2_cdr_serializer.h.
#ifdef MEMBER_ID_INVALID
#undef MEMBER_ID_INVALID
#endif

#include "ros2_type_support.h"
#include "ibmw_ros2_type_support.h"

namespace ibmw::ros2 {

/**
 * @brief ROS2 TypeSupport factory
 *
 * Manages creation of TypeSupport for all registered ROS2 message types.
 */
class ROS2TypeSupportFactory {
 public:
  using TypeSupportCreator = std::function<
      std::shared_ptr<eprosima::fastdds::dds::TopicDataType>()>;

  static ROS2TypeSupportFactory& Instance() {
    static ROS2TypeSupportFactory instance;
    return instance;
  }

  /**
   * @brief Register a TypeSupport creator
   *
   * @param ros2_type_name ROS2 type name (e.g., "std_msgs/msg/String")
   * @param creator TypeSupport creation function
   */
  void RegisterType(const std::string& ros2_type_name, TypeSupportCreator creator) {
    type_creators_[ros2_type_name] = std::move(creator);
  }

  /**
   * @brief Create a TypeSupport instance
   *
   * @param ros2_type_name ROS2 type name
   * @return TypeSupport shared_ptr, nullptr on failure
   */
  std::shared_ptr<eprosima::fastdds::dds::TopicDataType> CreateTypeSupport(
      const std::string& ros2_type_name) const {
    auto it = type_creators_.find(ros2_type_name);
    if (it == type_creators_.end()) {
      return nullptr;
    }
    return it->second();
  }

  /**
   * @brief Check if a type is registered
   */
  bool HasType(const std::string& ros2_type_name) const {
    return type_creators_.find(ros2_type_name) != type_creators_.end();
  }

  /**
   * @brief Get all registered type names
   */
  std::vector<std::string> GetRegisteredTypes() const {
    std::vector<std::string> types;
    types.reserve(type_creators_.size());
    for (const auto& [name, _] : type_creators_) {
      types.push_back(name);
    }
    return types;
  }

 private:
  ROS2TypeSupportFactory() = default;

  std::unordered_map<std::string, TypeSupportCreator> type_creators_;
};

/**
 * @brief Auto-registration helper for TypeSupport
 *
 * Usage:
 *   static TypeSupportRegistrar<std_msgs::msg::String> registrar(
 *     "std_msgs/msg/String",
 *     "std_msgs::msg::dds_::String_",
 *     false);
 */
template <typename ROS2MsgType>
class TypeSupportRegistrar {
 public:
  TypeSupportRegistrar(
      const std::string& ros2_type_name,
      const std::string& dds_type_name,
      bool is_plain) {
    static_cast<void>(is_plain);
    ROS2TypeSupportFactory::Instance().RegisterType(
        ros2_type_name,
        [dds_type_name]() {
          return std::make_shared<ROS2TypeSupport<ROS2MsgType>>(
              dds_type_name, false);
        });
  }
};

}  // namespace ibmw::ros2

// ========================================================================
// Implementation of GetIbmwTypeSupport — constructs ibmw_type_support_base_t
// directly from the template parameter + the given ros2_type_name.
//
// This does NOT use ROS2TypeSupportFactory (which is for extension-level
// dynamic type support only).  Instead it creates ROS2TypeSupport<T>
// directly.
// ========================================================================

namespace ibmw::ros2 {

namespace detail_impl {

// Convert a ROS2 type name like "example_ros2/msg/RosTestMsg" into the
// DDS type name convention: "example_ros2::msg::dds_::RosTestMsg_".
inline std::string Ros2TypeNameToDdsTypeName(const std::string& ros2_name) {
  auto last_slash = ros2_name.rfind('/');
  if (last_slash == std::string::npos) {
    return ros2_name;
  }
  std::string prefix = ros2_name.substr(0, last_slash);
  std::string type_name = ros2_name.substr(last_slash + 1);
  std::string dds_prefix;
  for (char c : prefix) {
    if (c == '/') {
      dds_prefix += "::";
    } else {
      dds_prefix += c;
    }
  }
  return dds_prefix + "::dds_::" + type_name + "_";
}

}  // namespace detail_impl

template <typename ROS2MsgType>
const ibmw_type_support_base_t* GetIbmwTypeSupport(const std::string& ros2_type_name) {
  // Derive the DDS type name
  static const std::string kDdsTypeName =
      detail_impl::Ros2TypeNameToDdsTypeName(ros2_type_name);

  // Wrap as eprosima::fastdds::dds::TypeSupport. TypeSupport owns the
  // TopicDataType pointer, so do not keep a second shared_ptr owner here.
  //
  // Keep the ROS2-compatible DDS lane on the typed-object path even for
  // fixed-size/trivially-copyable ROS2 messages such as std_msgs/msg/Bool and
  // std_msgs/msg/Int32.  Marking this handwritten TopicDataType as FastDDS
  // plain lets FastDDS engage its PlainCdr optimizations, but this type support
  // does not provide the full generated ROSIDL plain-type metadata/layout
  // contract.  The result is that stock ROS2 -> IBMW samples can be discovered
  // but never delivered to the user callback.  Native DDS types still keep
  // their dedicated plain fast path through dds_types::PlainDdsPubSubType.
  static eprosima::fastdds::dds::TypeSupport type_support(
      new ibmw::ros2::ROS2TypeSupport<ROS2MsgType>(kDdsTypeName, false));

  // ibmw type name: ros2:std_msgs/msg/String
  static const std::string kIbmwTypeName = "ros2:" + ros2_type_name;

  // Create ibmw TypeSupport
  static const ibmw_type_support_base_t kTs{
      .type_name = [](void*) -> ibmw_string_view_t {
        return ibmw::util::ToIbmwStringView(kIbmwTypeName);
      },

      .create = [](void*) -> void* {
        return new ROS2MsgType();
      },

      .destroy = [](void*, void* msg) {
        delete static_cast<ROS2MsgType*>(msg);
      },

      .copy = [](void*, const void* from, void* to) {
        *static_cast<ROS2MsgType*>(to) = *static_cast<const ROS2MsgType*>(from);
      },

      .move = [](void*, void* from, void* to) {
        *static_cast<ROS2MsgType*>(to) = std::move(*static_cast<ROS2MsgType*>(from));
      },

      // Native DDS mode: serialization not supported at ibmw level
      .serialize = [](void*, ibmw_string_view_t, const void*,
                      const ibmw_buffer_array_allocator_t*,
                      ibmw_buffer_array_t*) -> bool {
        return false;
      },

      .deserialize = [](void*, ibmw_string_view_t,
                        ibmw_buffer_array_view_t, void*) -> bool {
        return false;
      },

      .serialization_types_supported_num = [](void*) -> size_t { return 0; },

      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* {
        return nullptr;
      },

      // Return FastDDS TypeSupport for DDS driver
      .custom_type_support_ptr = [](void*) -> const void* {
        return &type_support;
      },

      .ctx = nullptr};

  return &kTs;
}

}  // namespace ibmw::ros2

// ========================================================================
// Convenience wrapper: ibmw::GetRos2MessageTypeSupport<T>()
// Used by the ROS2 RPC code generator (ros2_gen_ibmw_cpp_rpc.py) and
// type_support_pkg_main.cc files.
//
// This constructs the ibmw_type_support_base_t DIRECTLY from the message
// type, using rosidl_generator_traits::name<T>() for the human-readable
// ROS2 type name.  It does NOT go through ROS2TypeSupportFactory (which
// is only for extension-level dynamic type support).
// ========================================================================

#include <rosidl_runtime_cpp/traits.hpp>

namespace ibmw {

template <typename ROS2MsgType>
const ibmw_type_support_base_t* GetRos2MessageTypeSupport() {
  // Use rosidl_generator_traits::name<T>() to get the proper ROS2 type name
  // (e.g. "example_ros2/msg/RosTestMsg", "example_ros2/srv/RosTestRpc_Request")
  static const std::string kRos2TypeName = rosidl_generator_traits::name<ROS2MsgType>();

  // Derive the DDS type name for FastDDS TopicDataType
  static const std::string kDdsTypeName =
      ibmw::ros2::detail_impl::Ros2TypeNameToDdsTypeName(kRos2TypeName);

  // Wrap as eprosima::fastdds::dds::TypeSupport. TypeSupport owns the
  // TopicDataType pointer, so do not keep a second shared_ptr owner here.
  // See ibmw::ros2::GetIbmwTypeSupport above: handwritten ROS2 TopicDataType
  // instances intentionally stay off FastDDS PlainCdr optimizations.
  static eprosima::fastdds::dds::TypeSupport type_support(
      new ibmw::ros2::ROS2TypeSupport<ROS2MsgType>(kDdsTypeName, false));

  // ibmw type name: ros2:example_ros2/msg/RosTestMsg
  static const std::string kIbmwTypeName = "ros2:" + kRos2TypeName;

  // Build the ibmw type support struct inline
  static const ibmw_type_support_base_t kTs{
      .type_name = [](void*) -> ibmw_string_view_t {
        return ibmw::util::ToIbmwStringView(kIbmwTypeName);
      },

      .create = [](void*) -> void* {
        return new ROS2MsgType();
      },

      .destroy = [](void*, void* msg) {
        delete static_cast<ROS2MsgType*>(msg);
      },

      .copy = [](void*, const void* from, void* to) {
        *static_cast<ROS2MsgType*>(to) = *static_cast<const ROS2MsgType*>(from);
      },

      .move = [](void*, void* from, void* to) {
        *static_cast<ROS2MsgType*>(to) = std::move(*static_cast<ROS2MsgType*>(from));
      },

      // Serialization handled natively by DDS/ROS2 — not through ibmw
      .serialize = [](void*, ibmw_string_view_t, const void*,
                      const ibmw_buffer_array_allocator_t*,
                      ibmw_buffer_array_t*) -> bool {
        return false;
      },

      .deserialize = [](void*, ibmw_string_view_t,
                        ibmw_buffer_array_view_t, void*) -> bool {
        return false;
      },

      .serialization_types_supported_num = [](void*) -> size_t { return 0; },

      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* {
        return nullptr;
      },

      // Return FastDDS TypeSupport for DDS driver
      .custom_type_support_ptr = [](void*) -> const void* {
        return &type_support;
      },

      .ctx = nullptr};

  return &kTs;
}

}  // namespace ibmw
