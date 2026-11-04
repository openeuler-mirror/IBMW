// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "call_type_support.h"
#include "extension_api/ibmw_extension_export.h"

#include <regex>
#include <rclcpp/typesupport_helpers.hpp>
#include <rosidl_typesupport_introspection_cpp/service_introspection.hpp>
#include "type_support/ros2/ros2_text_serialization.h"

namespace ibmw::cli::service {
namespace {
using Members = rosidl_typesupport_introspection_cpp::MessageMembers;
struct MessageType {
  std::string name;
  const Members* members;
  ibmw_type_support_base_t base{};
  explicit MessageType(std::string type, const Members* fields) : name(std::move(type)), members(fields) {
    if (!members || !members->init_function || !members->fini_function || !members->size_of_) throw std::runtime_error("Incomplete ROS 2 message introspection.");
    base = {
      .type_name = [](void* p) { return ibmw::util::ToIbmwStringView(static_cast<MessageType*>(p)->name); },
      .create = [](void* p) -> void* {
        const auto* m = static_cast<MessageType*>(p)->members;
        auto* message = ::operator new(m->size_of_);
        try { m->init_function(message, rosidl_runtime_cpp::MessageInitialization::ALL); }
        catch (...) { ::operator delete(message); throw; }
        return message;
      },
      .destroy = [](void* p, void* message) { static_cast<MessageType*>(p)->members->fini_function(message); ::operator delete(message); },
      .copy = [](void* p, const void* from, void* to) {
        const auto& m = *static_cast<MessageType*>(p)->members;
        ibmw::ros2::text_detail::ReadMessage(ibmw::ros2::text_detail::WriteMessage(from, m), to, m);
      },
      .move = [](void* p, void* from, void* to) {
        const auto& m = *static_cast<MessageType*>(p)->members;
        ibmw::ros2::text_detail::ReadMessage(ibmw::ros2::text_detail::WriteMessage(from, m), to, m);
      },
      .serialize = [](void* p, ibmw_string_view_t format, const void* message, const ibmw_buffer_array_allocator_t* allocator, ibmw_buffer_array_t* buffers) {
        try {
          if (ibmw::util::ToStdStringView(format) != "json") return false;
          return ibmw::util::text::Store(ibmw::ros2::text_detail::WriteMessage(message, *static_cast<MessageType*>(p)->members), allocator, buffers);
        } catch (...) { return false; }
      },
      .deserialize = [](void* p, ibmw_string_view_t format, ibmw_buffer_array_view_t buffers, void* message) {
        try {
          if (ibmw::util::ToStdStringView(format) != "json") return false;
          ibmw::ros2::text_detail::ReadMessage(ibmw::util::text::Parse(buffers), message, *static_cast<MessageType*>(p)->members);
          return true;
        } catch (...) { return false; }
      },
      .serialization_types_supported_num = [](void*) -> size_t { return 1; },
      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { static const ibmw_string_view_t formats[] = {{"json", 4}}; return formats; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; }, .ctx = this};
  }
};
struct Ros2CallTypes final : NativeCallTypes {
  std::shared_ptr<rcpputils::SharedLibrary> service_library, introspection_library;
  std::unique_ptr<MessageType> request_type, response_type;
  explicit Ros2CallTypes(const std::string& type) {
    static const std::regex pattern("([a-zA-Z][a-zA-Z0-9_]*)/srv/([a-zA-Z][a-zA-Z0-9_]*)");
    std::smatch match;
    if (!std::regex_match(type, match, pattern)) throw std::invalid_argument("ROS 2 type must be pkg/srv/Type.");
    const std::string suffix = "__get_service_type_support_handle__" + match[1].str() + "__srv__" + match[2].str();
    using Get = const rosidl_service_type_support_t* (*)();
    service_library = rclcpp::get_typesupport_library(type, "rosidl_typesupport_cpp");
    introspection_library = rclcpp::get_typesupport_library(type, "rosidl_typesupport_introspection_cpp");
    service = reinterpret_cast<Get>(service_library->get_symbol("rosidl_typesupport_cpp" + suffix))();
    const auto* introspection = reinterpret_cast<Get>(introspection_library->get_symbol("rosidl_typesupport_introspection_cpp" + suffix))();
    if (!service || !introspection || !introspection->data) throw std::runtime_error("Missing native ROS 2 service TypeSupport.");
    const auto* members = static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers*>(introspection->data);
    request_type = std::make_unique<MessageType>("ros2:" + type + "_Request", members->request_members_);
    response_type = std::make_unique<MessageType>("ros2:" + type + "_Response", members->response_members_);
    request = ibmw::util::TypeSupportRef(&request_type->base); response = ibmw::util::TypeSupportRef(&response_type->base);
  }
};
}  // namespace
}  // namespace ibmw::cli::service

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw::cli::service::NativeCallTypes* IbmwServiceCallRos2TypesV1(const char* type) {
  return new ibmw::cli::service::Ros2CallTypes(type);
}
