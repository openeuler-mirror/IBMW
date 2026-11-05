// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "param_command.h"

#include <algorithm>
#include <iostream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "call_client.h"
#include "cli/command.h"
#include "cpp/service/rpc_handle.h"
#include "parameter_ext/parameter_type_support.h"

namespace ibmw::cli::param {
namespace {
namespace protocol = ibmw::protocols::parameter_extension;
using ibmw::extensions::parameter_extension::GetParameterTypeSupport;

struct CallFailure { int exit_code; };

void ValidateOptions(const Invocation& invocation) {
  for (const auto& [name, values] : invocation.options) {
    if (name != "--param-prefixes" && values.size() != 1)
      throw std::invalid_argument(name + " must be specified once.");
  }
  unsigned backends = 0;
  for (const auto* flag : {"--dds", "--http", "--grpc", "--mqtt", "--zenoh"})
    backends += invocation.HasOption(flag);
  if (backends != 1)
    throw std::invalid_argument("param requires exactly one backend: --dds, --http, --grpc, --mqtt or --zenoh.");
  for (size_t index = 0; index < std::min(size_t{2}, invocation.positional_args.size()); ++index) {
    if (invocation.positional_args[index].empty())
      throw std::invalid_argument("Module and parameter names must not be empty.");
  }
  const auto name = invocation.HasOption("--service")
      ? invocation.options.at("--service").front() : protocol::kServiceName;
  if (name.empty() || name.find_first_of(": \t\r\n?#") != std::string::npos)
    throw std::invalid_argument("--service requires a service base name, without a type prefix or method.");
}

template <typename Req, typename Rsp>
Rsp Call(const Invocation& invocation, std::string_view method, const Req& request) {
  Invocation rpc = invocation;
  for (const auto* name : {"--service", "--hide-type", "--filter", "--param-type", "--param-prefixes"})
    rpc.options.erase(name);
  const auto service_name = invocation.OptionValue("--service", protocol::kServiceName);
  const auto function = ibmw::rpc::GetFullFuncName("json", service_name, method);
  const auto json_request = nlohmann::json(request).dump();
  rpc.positional_args = {function, {}, json_request};
  // ParseCallOptions validates the generic service type label. Actual protocol
  // TypeSupport is built in below, so users never provide a library or type pair.
  rpc.positional_args[1] = std::string(Req::kTypeName) + " -> " + Rsp::kTypeName;
  auto options = service::ParseCallOptions(rpc);
  service::CallTypes types;
  types.function = function;
  types.request = ibmw::util::TypeSupportRef(GetParameterTypeSupport<Req>());
  types.response = ibmw::util::TypeSupportRef(GetParameterTypeSupport<Rsp>());
  if (options.backend == "dds" &&
      (!types.request.CustomTypeSupportPtr() || !types.response.CustomTypeSupportPtr()))
    throw std::runtime_error("Parameter DDS support is not built; rebuild with IBMW_BUILD_DDS_EXTENSION=ON.");
  types.serialization = options.serialization.empty()
      ? (options.backend == "dds" ? "cdr" : "json") : options.serialization;
  if (!types.request.CheckSerializationTypeSupported(types.serialization) ||
      !types.response.CheckSerializationTypeSupported(types.serialization))
    throw std::invalid_argument("The selected serialization is not supported by the parameter protocol.");
  const auto result = service::ExecuteCall(std::move(options), std::move(types), json_request, "ibmw param");
  if (result.exit_code != 0) throw CallFailure{result.exit_code};
  const auto json = nlohmann::json::parse(result.response);
  // Do not accept missing status fields as a successful response.
  const auto code = json.at("code").get<uint64_t>();
  const auto reason = json.at("msg").get<std::string>();
  if (code != 0) throw std::runtime_error(std::string(method) + " failed: " + reason +
                                        " (code " + std::to_string(code) + ").");
  return json.get<Rsp>();
}

int List(const Invocation& invocation) {
  ValidateOptions(invocation);
  std::optional<std::regex> filter;
  if (invocation.HasOption("--filter")) filter.emplace(invocation.OptionValue("--filter"));
  const auto prefixes = invocation.options.find("--param-prefixes");
  std::vector<std::string> modules;
  const bool all = invocation.positional_args.empty();
  if (all) {
    modules = Call<protocol::ListModulesReq, protocol::ListModulesRsp>(
        invocation, "ListModules", {}).module_names;
  } else {
    modules = {invocation.positional_args.front()};
  }
  std::sort(modules.begin(), modules.end());
  std::vector<std::pair<std::string, std::vector<std::string>>> groups;
  for (const auto& module : modules) {
    auto keys = Call<protocol::ListParameterReq, protocol::ListParameterRsp>(
        invocation, "List", {.module_name = module}).parameter_keys;
    std::sort(keys.begin(), keys.end());
    std::erase_if(keys, [&](const std::string& key) {
      if (filter && !std::regex_search(key, *filter, std::regex_constants::match_continuous)) return true;
      if (prefixes == invocation.options.end()) return false;
      return std::none_of(prefixes->second.begin(), prefixes->second.end(), [&](const auto& prefix) {
        return prefix.empty() || key == prefix || key.starts_with(prefix + ".");
      });
    });
    if (!keys.empty() || !all) groups.emplace_back(module, std::move(keys));
  }
  // Print after all calls complete: never silently present a partial list as complete.
  for (const auto& [module, keys] : groups) {
    if (all) std::cout << module << ":\n";
    for (const auto& key : keys)
      std::cout << (all ? "  " : "") << key
                << (invocation.HasOption("--param-type") ? " (string)" : "") << '\n';
  }
  return 0;
}

int Get(const Invocation& invocation) {
  ValidateOptions(invocation);
  const auto result = Call<protocol::GetParameterReq, protocol::GetParameterRsp>(
      invocation, "Get", {.module_name = invocation.positional_args[0],
                           .parameter_key = invocation.positional_args[1]});
  if (!invocation.HasOption("--hide-type")) std::cout << "String value is: ";
  std::cout << result.parameter_value << '\n';
  return 0;
}

int Set(const Invocation& invocation) {
  ValidateOptions(invocation);
  Call<protocol::SetParameterReq, protocol::SetParameterRsp>(
      invocation, "Set", {.module_name = invocation.positional_args[0],
                           .parameter_key = invocation.positional_args[1],
                           .parameter_value = invocation.positional_args[2]});
  std::cout << "Set parameter successful\n";
  return 0;
}

void AddOptions(Command& command) {
  service::AddCallTransportOptions(command, false);
  command.AddOption("--service", {}, "name",
      "Parameter service base name (default: " + std::string(protocol::kServiceName) + ")",
      OptionValueMode::kRequired);
}
}  // namespace
}  // namespace ibmw::cli::param

int RunParamCommand(int argc, const char* const* argv) {
  using namespace ibmw::cli;
  try {
    Command command("ibmw param", "Manage runtime string parameters using one RPC backend; set does not save files");
    auto& list = command.AddSubcommand("list", "List parameters in one module, or all modules of the selected parameter service");
    list.AddArgument("[module]", "Module name", false).SetHandler(param::List);
    list.AddOption("--filter", {}, "regex", "Only show parameter names matching this regex", OptionValueMode::kRequired);
    list.AddOption("--param-prefixes", {}, "prefix", "Only show this dotted namespace prefix; repeatable", OptionValueMode::kRequired);
    list.AddOption("--param-type", {}, {}, "Show parameter types");
    param::AddOptions(list);
    auto& get = command.AddSubcommand("get", "Read a module parameter");
    get.AddArgument("<module>", "Module name").AddArgument("<parameter_name>", "Parameter name").SetHandler(param::Get);
    get.AddOption("--hide-type", {}, {}, "Only print the parameter value");
    param::AddOptions(get);
    auto& set = command.AddSubcommand("set", "Set or create a string parameter in an existing module (no file writes)");
    set.AddArgument("<module>", "Module name").AddArgument("<parameter_name>", "Parameter name")
       .AddArgument("<value>", "Literal string value; use -- before a value beginning with '-'").SetHandler(param::Set);
    param::AddOptions(set);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    std::vector<char*> mutable_args;
    for (auto& arg : args) mutable_args.push_back(arg.data());
    return command.Run(static_cast<int>(mutable_args.size()), mutable_args.data());
  } catch (const param::CallFailure& failure) {
    return failure.exit_code;
  } catch (const std::exception& error) {
    std::cerr << "ibmw param: error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "ibmw param: error: Unknown failure.\n";
  }
  return 1;
}
