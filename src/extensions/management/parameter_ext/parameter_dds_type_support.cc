// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "parameter_ext/parameter_type_support.h"
#include "Parameter.hpp"
#include "ParameterCdrAux.hpp"
#include "type_support/dds/fastcdr_v1_compat.h"
#include "ParameterCdrAux.ipp"
#include "cpp/util/json_type_support.h"

namespace ibmw::extensions::parameter_extension {
namespace {
namespace protocol = ibmw::protocols::parameter_extension;
namespace wire = ibmw::protocols::parameter_dds;
wire::SetParameterReq ToWire(const protocol::SetParameterReq& value) {
  wire::SetParameterReq result;
  result.module_name(value.module_name);
  result.parameter_key(value.parameter_key);
  result.parameter_value(value.parameter_value);
  return result;
}
void FromWire(const wire::SetParameterReq& value, protocol::SetParameterReq& result) {
  result.module_name = value.module_name();
  result.parameter_key = value.parameter_key();
  result.parameter_value = value.parameter_value();
}
wire::SetParameterRsp ToWire(const protocol::SetParameterRsp& value) {
  wire::SetParameterRsp result;
  result.code(value.code);
  result.msg(value.msg);
  return result;
}
void FromWire(const wire::SetParameterRsp& value, protocol::SetParameterRsp& result) {
  result.code = value.code();
  result.msg = value.msg();
}
wire::GetParameterReq ToWire(const protocol::GetParameterReq& value) {
  wire::GetParameterReq result;
  result.module_name(value.module_name);
  result.parameter_key(value.parameter_key);
  return result;
}
void FromWire(const wire::GetParameterReq& value, protocol::GetParameterReq& result) {
  result.module_name = value.module_name();
  result.parameter_key = value.parameter_key();
}
wire::GetParameterRsp ToWire(const protocol::GetParameterRsp& value) {
  wire::GetParameterRsp result;
  result.code(value.code);
  result.msg(value.msg);
  result.parameter_value(value.parameter_value);
  return result;
}
void FromWire(const wire::GetParameterRsp& value, protocol::GetParameterRsp& result) {
  result.code = value.code();
  result.msg = value.msg();
  result.parameter_value = value.parameter_value();
}
wire::ListParameterReq ToWire(const protocol::ListParameterReq& value) {
  wire::ListParameterReq result;
  result.module_name(value.module_name);
  return result;
}
void FromWire(const wire::ListParameterReq& value, protocol::ListParameterReq& result) {
  result.module_name = value.module_name();
}
wire::ListParameterRsp ToWire(const protocol::ListParameterRsp& value) {
  wire::ListParameterRsp result;
  result.code(value.code);
  result.msg(value.msg);
  result.parameter_keys(value.parameter_keys);
  return result;
}
void FromWire(const wire::ListParameterRsp& value, protocol::ListParameterRsp& result) {
  result.code = value.code();
  result.msg = value.msg();
  result.parameter_keys = value.parameter_keys();
}
wire::DumpParameterReq ToWire(const protocol::DumpParameterReq& value) {
  wire::DumpParameterReq result;
  result.module_names(value.module_names);
  return result;
}
void FromWire(const wire::DumpParameterReq& value, protocol::DumpParameterReq& result) {
  result.module_names = value.module_names();
}
wire::DumpParameterRsp ToWire(const protocol::DumpParameterRsp& value) {
  wire::DumpParameterRsp result;
  result.code(value.code);
  result.msg(value.msg);
  result.module_parameter_json(nlohmann::json(value.module_parameter_map).dump());
  return result;
}
void FromWire(const wire::DumpParameterRsp& value, protocol::DumpParameterRsp& result) {
  result.code = value.code();
  result.msg = value.msg();
  result.module_parameter_map = nlohmann::json::parse(value.module_parameter_json())
      .get<std::map<std::string, protocol::ParameterMap>>();
}
wire::LoadParameterReq ToWire(const protocol::LoadParameterReq& value) {
  wire::LoadParameterReq result;
  result.module_parameter_json(nlohmann::json(value.module_parameter_map).dump());
  return result;
}
void FromWire(const wire::LoadParameterReq& value, protocol::LoadParameterReq& result) {
  result.module_parameter_map = nlohmann::json::parse(value.module_parameter_json())
      .get<std::map<std::string, protocol::ParameterMap>>();
}
wire::LoadParameterRsp ToWire(const protocol::LoadParameterRsp& value) {
  wire::LoadParameterRsp result;
  result.code(value.code);
  result.msg(value.msg);
  return result;
}
void FromWire(const wire::LoadParameterRsp& value, protocol::LoadParameterRsp& result) {
  result.code = value.code();
  result.msg = value.msg();
}
wire::ListModulesReq ToWire(const protocol::ListModulesReq&) { return {}; }
void FromWire(const wire::ListModulesReq&, protocol::ListModulesReq&) {}
wire::ListModulesRsp ToWire(const protocol::ListModulesRsp& value) {
  wire::ListModulesRsp result;
  result.code(value.code);
  result.msg(value.msg);
  result.module_names(value.module_names);
  return result;
}
void FromWire(const wire::ListModulesRsp& value, protocol::ListModulesRsp& result) {
  result.code = value.code();
  result.msg = value.msg();
  result.module_names = value.module_names();
}
}  // namespace
}  // namespace ibmw::extensions::parameter_extension

// The runtime still owns the original C++/JSON structs. Only the wire codec
// converts to generated DDS structs; no backend-specific types leak to modules.
namespace eprosima::fastcdr {
namespace parameter = ibmw::extensions::parameter_extension;
namespace protocol = ibmw::protocols::parameter_extension;
namespace wire = ibmw::protocols::parameter_dds;
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::SetParameterReq& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::SetParameterReq& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::SetParameterReq& value) {
  wire::SetParameterReq native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::SetParameterRsp& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::SetParameterRsp& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::SetParameterRsp& value) {
  wire::SetParameterRsp native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::GetParameterReq& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::GetParameterReq& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::GetParameterReq& value) {
  wire::GetParameterReq native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::GetParameterRsp& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::GetParameterRsp& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::GetParameterRsp& value) {
  wire::GetParameterRsp native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::ListParameterReq& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::ListParameterReq& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::ListParameterReq& value) {
  wire::ListParameterReq native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::ListParameterRsp& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::ListParameterRsp& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::ListParameterRsp& value) {
  wire::ListParameterRsp native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::DumpParameterReq& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::DumpParameterReq& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::DumpParameterReq& value) {
  wire::DumpParameterReq native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::DumpParameterRsp& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::DumpParameterRsp& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::DumpParameterRsp& value) {
  wire::DumpParameterRsp native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::LoadParameterReq& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::LoadParameterReq& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::LoadParameterReq& value) {
  wire::LoadParameterReq native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::LoadParameterRsp& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::LoadParameterRsp& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::LoadParameterRsp& value) {
  wire::LoadParameterRsp native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::ListModulesReq& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::ListModulesReq& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::ListModulesReq& value) {
  wire::ListModulesReq native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
template <>
size_t calculate_serialized_size(CdrSizeCalculator& calculator, const protocol::ListModulesRsp& value, size_t& alignment) {
  return calculator.calculate_serialized_size(parameter::ToWire(value), alignment);
}
template <>
void serialize(Cdr& cdr, const protocol::ListModulesRsp& value) {
  serialize(cdr, parameter::ToWire(value));
}
template <>
void deserialize(Cdr& cdr, protocol::ListModulesRsp& value) {
  wire::ListModulesRsp native;
  deserialize(cdr, native);
  parameter::FromWire(native, value);
}
}  // namespace eprosima::fastcdr

#include "type_support/dds/dds_type_support.h"

namespace ibmw::extensions::parameter_extension {
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
const ibmw_type_support_base_t* FindParameterTypeSupport(std::string_view name) {
  using namespace ibmw::protocols::parameter_extension;
  if (name == SetParameterReq::kTypeName)
    return NativeType<SetParameterReq>("ibmw::protocols::parameter_dds::SetParameterReq");
  if (name == SetParameterRsp::kTypeName)
    return NativeType<SetParameterRsp>("ibmw::protocols::parameter_dds::SetParameterRsp");
  if (name == GetParameterReq::kTypeName)
    return NativeType<GetParameterReq>("ibmw::protocols::parameter_dds::GetParameterReq");
  if (name == GetParameterRsp::kTypeName)
    return NativeType<GetParameterRsp>("ibmw::protocols::parameter_dds::GetParameterRsp");
  if (name == ListParameterReq::kTypeName)
    return NativeType<ListParameterReq>("ibmw::protocols::parameter_dds::ListParameterReq");
  if (name == ListParameterRsp::kTypeName)
    return NativeType<ListParameterRsp>("ibmw::protocols::parameter_dds::ListParameterRsp");
  if (name == DumpParameterReq::kTypeName)
    return NativeType<DumpParameterReq>("ibmw::protocols::parameter_dds::DumpParameterReq");
  if (name == DumpParameterRsp::kTypeName)
    return NativeType<DumpParameterRsp>("ibmw::protocols::parameter_dds::DumpParameterRsp");
  if (name == LoadParameterReq::kTypeName)
    return NativeType<LoadParameterReq>("ibmw::protocols::parameter_dds::LoadParameterReq");
  if (name == LoadParameterRsp::kTypeName)
    return NativeType<LoadParameterRsp>("ibmw::protocols::parameter_dds::LoadParameterRsp");
  if (name == ListModulesReq::kTypeName)
    return NativeType<ListModulesReq>("ibmw::protocols::parameter_dds::ListModulesReq");
  if (name == ListModulesRsp::kTypeName)
    return NativeType<ListModulesRsp>("ibmw::protocols::parameter_dds::ListModulesRsp");
  throw std::invalid_argument("Unknown parameter protocol type.");
}
}  // namespace ibmw::extensions::parameter_extension
