// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "log_control_ext/log_control_type_support.h"
#include "LogControl.hpp"
#include "LogControlCdrAux.hpp"
#include "type_support/dds/fastcdr_v1_compat.h"
#include "LogControlCdrAux.ipp"
#include "cpp/util/json_type_support.h"

namespace ibmw::extensions::log_control_extension {
namespace {
namespace protocol = ibmw::protocols::log_control_extension;
namespace wire = ibmw::protocols::log_control_dds;
wire::GetModuleLogLevelReq ToWire(const protocol::GetModuleLogLevelReq& value) {
  wire::GetModuleLogLevelReq result;
  result.module_names(value.module_names);
  return result;
}
void FromWire(const wire::GetModuleLogLevelReq& value, protocol::GetModuleLogLevelReq& result) {
  result.module_names = value.module_names();
}
wire::GetModuleLogLevelRsp ToWire(const protocol::GetModuleLogLevelRsp& value) {
  wire::GetModuleLogLevelRsp result;
  result.code(value.code);
  result.msg(value.msg);
  result.module_log_level_json(nlohmann::json(value.module_log_level_map).dump());
  return result;
}
void FromWire(const wire::GetModuleLogLevelRsp& value, protocol::GetModuleLogLevelRsp& result) {
  result.code = value.code();
  result.msg = value.msg();
  result.module_log_level_map = nlohmann::json::parse(value.module_log_level_json())
      .get<std::map<std::string, std::string>>();
}
wire::SetModuleLogLevelReq ToWire(const protocol::SetModuleLogLevelReq& value) {
  wire::SetModuleLogLevelReq result;
  result.module_log_level_json(nlohmann::json(value.module_log_level_map).dump());
  return result;
}
void FromWire(const wire::SetModuleLogLevelReq& value, protocol::SetModuleLogLevelReq& result) {
  result.module_log_level_map = nlohmann::json::parse(value.module_log_level_json())
      .get<std::map<std::string, std::string>>();
}
wire::SetModuleLogLevelRsp ToWire(const protocol::SetModuleLogLevelRsp& value) {
  wire::SetModuleLogLevelRsp result;
  result.code(value.code);
  result.msg(value.msg);
  return result;
}
void FromWire(const wire::SetModuleLogLevelRsp& value, protocol::SetModuleLogLevelRsp& result) {
  result.code = value.code();
  result.msg = value.msg();
}
}  // namespace
}  // namespace ibmw::extensions::log_control_extension

// Adapt only at the wire boundary; runtime objects keep their existing ABI.
namespace eprosima::fastcdr {
namespace log_control = ibmw::extensions::log_control_extension;
namespace protocol = ibmw::protocols::log_control_extension;
namespace wire = ibmw::protocols::log_control_dds;
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::GetModuleLogLevelReq& value, size_t& alignment) {
  return calculator.calculate_serialized_size(log_control::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::GetModuleLogLevelReq& value) {
  serialize(cdr, log_control::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::GetModuleLogLevelReq& value) {
  wire::GetModuleLogLevelReq native;
  deserialize(cdr, native);
  log_control::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::GetModuleLogLevelRsp& value, size_t& alignment) {
  return calculator.calculate_serialized_size(log_control::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::GetModuleLogLevelRsp& value) {
  serialize(cdr, log_control::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::GetModuleLogLevelRsp& value) {
  wire::GetModuleLogLevelRsp native;
  deserialize(cdr, native);
  log_control::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::SetModuleLogLevelReq& value, size_t& alignment) {
  return calculator.calculate_serialized_size(log_control::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::SetModuleLogLevelReq& value) {
  serialize(cdr, log_control::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::SetModuleLogLevelReq& value) {
  wire::SetModuleLogLevelReq native;
  deserialize(cdr, native);
  log_control::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::SetModuleLogLevelRsp& value, size_t& alignment) {
  return calculator.calculate_serialized_size(log_control::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::SetModuleLogLevelRsp& value) {
  serialize(cdr, log_control::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::SetModuleLogLevelRsp& value) {
  wire::SetModuleLogLevelRsp native;
  deserialize(cdr, native);
  log_control::FromWire(native, value);
}
}  // namespace eprosima::fastcdr

#include "type_support/dds/dds_type_support.h"

namespace ibmw::extensions::log_control_extension {
namespace {
template <typename T>
const ibmw_type_support_base_t* NativeType(std::string_view name) {
  static const auto support = [name] {
    auto result = *ibmw::dds::GetDdsMessageTypeSupport<T>(std::string(name));
    // Preserve the existing JSON protocol identity and object ABI on every backend.
    result.type_name = ibmw::json::GetJsonMessageTypeSupport<T>()->type_name;
    // These legacy structs use ADL JSON conversion, not generated DDS JSON codecs.
    result.serialize = [](void* ctx, ibmw_string_view_t format, const void* message,
                          const ibmw_buffer_array_allocator_t* allocator,
                          ibmw_buffer_array_t* buffers) {
      const auto* codec = std::string_view(format.str, format.len) == "json"
          ? ibmw::json::GetJsonMessageTypeSupport<T>()
          : ibmw::dds::GetDdsMessageTypeSupport<T>();
      return codec->serialize(codec->ctx, format, message, allocator, buffers);
    };
    result.deserialize = [](void* ctx, ibmw_string_view_t format,
                            ibmw_buffer_array_view_t buffers, void* message) {
      const auto* codec = std::string_view(format.str, format.len) == "json"
          ? ibmw::json::GetJsonMessageTypeSupport<T>()
          : ibmw::dds::GetDdsMessageTypeSupport<T>();
      return codec->deserialize(codec->ctx, format, buffers, message);
    };
    return result;
  }();
  return &support;
}
}  // namespace
const ibmw_type_support_base_t* FindLogControlTypeSupport(std::string_view name) {
  using namespace ibmw::protocols::log_control_extension;
  if (name == GetModuleLogLevelReq::kTypeName)
    return NativeType<GetModuleLogLevelReq>("ibmw::protocols::log_control_dds::GetModuleLogLevelReq");
  if (name == GetModuleLogLevelRsp::kTypeName)
    return NativeType<GetModuleLogLevelRsp>("ibmw::protocols::log_control_dds::GetModuleLogLevelRsp");
  if (name == SetModuleLogLevelReq::kTypeName)
    return NativeType<SetModuleLogLevelReq>("ibmw::protocols::log_control_dds::SetModuleLogLevelReq");
  if (name == SetModuleLogLevelRsp::kTypeName)
    return NativeType<SetModuleLogLevelRsp>("ibmw::protocols::log_control_dds::SetModuleLogLevelRsp");
  throw std::invalid_argument("Unknown log control protocol type.");
}
}  // namespace ibmw::extensions::log_control_extension
