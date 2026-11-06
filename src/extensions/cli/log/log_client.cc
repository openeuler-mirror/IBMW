// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "log_client.h"

#include <stdexcept>
#include <type_traits>
#include <utility>

#include "call_client.h"
#include "cpp/service/rpc_handle.h"
#include "log_control_ext/log_control_type_support.h"

namespace ibmw::cli::log {
namespace {
namespace protocol = ibmw::protocols::log_control_extension;
using ibmw::extensions::log_control_extension::GetLogControlTypeSupport;

void ValidateOptions(const Invocation& invocation) {
  for (const auto& [name, values] : invocation.options) {
    if (values.size() != 1) throw std::invalid_argument(name + " must be specified once.");
  }
  unsigned backends = 0;
  for (const auto* flag : {"--dds", "--http", "--grpc", "--mqtt", "--zenoh"})
    backends += invocation.HasOption(flag);
  if (backends != 1)
    throw std::invalid_argument("log requires exactly one backend: --dds, --http, --grpc, --mqtt or --zenoh.");
  const auto name = invocation.HasOption("--service")
      ? invocation.options.at("--service").front() : protocol::kServiceName;
  if (name.empty() || name.find_first_of(": \t\r\n?#") != std::string::npos)
    throw std::invalid_argument("--service requires a service base name, without a type prefix or method.");
}

template <typename Req, typename Rsp>
Rsp Call(const Invocation& invocation, std::string_view method, const Req& request,
         std::string_view diagnostic_name) {
  Invocation rpc = invocation;
  rpc.options.erase("--service");
  const auto function = ibmw::rpc::GetFullFuncName(
      "json", invocation.OptionValue("--service", protocol::kServiceName), method);
  const auto json_request = nlohmann::json(request).dump();
  rpc.positional_args = {function, std::string(Req::kTypeName) + " -> " + Rsp::kTypeName, json_request};
  auto options = service::ParseCallOptions(rpc);
  service::CallTypes types;
  types.function = function;
  types.request = ibmw::util::TypeSupportRef(GetLogControlTypeSupport<Req>());
  types.response = ibmw::util::TypeSupportRef(GetLogControlTypeSupport<Rsp>());
  if (options.backend == "dds" &&
      (!types.request.CustomTypeSupportPtr() || !types.response.CustomTypeSupportPtr()))
    throw std::runtime_error("Log DDS support is not built; rebuild with IBMW_BUILD_DDS_EXTENSION=ON.");
  types.serialization = options.serialization.empty()
      ? (options.backend == "dds" ? "cdr" : "json") : options.serialization;
  if (!types.request.CheckSerializationTypeSupported(types.serialization) ||
      !types.response.CheckSerializationTypeSupported(types.serialization))
    throw std::invalid_argument("The selected serialization is not supported by the log protocol.");
  const auto result = service::ExecuteCall(std::move(options), std::move(types), json_request, diagnostic_name);
  if (result.exit_code != 0) throw CallFailure{result.exit_code};
  const auto json = nlohmann::json::parse(result.response);
  const auto code = json.at("code").get<uint64_t>();
  const auto reason = json.at("msg").get<std::string>();
  if (code != 0) throw std::runtime_error(std::string(method) + " failed: " + reason +
                                        " (code " + std::to_string(code) + ").");
  if constexpr (std::is_same_v<Rsp, protocol::GetModuleLogLevelRsp>)
    (void)json.at("module_log_level_map");  // Missing data is not a successful empty list.
  return json.get<Rsp>();
}
}  // namespace

std::map<std::string, std::string> QueryLogLevels(
    const Invocation& connection, const std::vector<std::string>& module_names,
    std::string_view diagnostic_name) {
  ValidateOptions(connection);
  for (const auto& name : module_names)
    if (name.empty()) throw std::invalid_argument("Module name must not be empty.");
  auto response = Call<protocol::GetModuleLogLevelReq, protocol::GetModuleLogLevelRsp>(
      connection, "GetModuleLogLevel", {module_names}, diagnostic_name);
  for (auto& [name, value] : response.module_log_level_map) {
    const auto level = protocol::NormalizeLogLevel(value);
    if (!level) throw std::runtime_error("Invalid log level returned for " + name + ".");
    value = *level;
  }
  return std::move(response.module_log_level_map);
}

void SetLogLevel(const Invocation& connection, std::string_view module_name,
                 std::string_view value, std::string_view diagnostic_name) {
  ValidateOptions(connection);
  if (module_name.empty()) throw std::invalid_argument("Module name must not be empty.");
  const auto level = protocol::NormalizeLogLevel(value);
  if (!level) throw std::invalid_argument("Level must be TRACE, DEBUG, INFO, WARN, ERROR, FATAL or OFF (case-insensitive).");
  Call<protocol::SetModuleLogLevelReq, protocol::SetModuleLogLevelRsp>(
      connection, "SetModuleLogLevel", {{{std::string(module_name), std::string(*level)}}}, diagnostic_name);
}

}  // namespace ibmw::cli::log
