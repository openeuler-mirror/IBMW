// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "call_command.h"
#include "call_client.h"

#include <iostream>
#include "cpp/service/rpc_handle.h"

namespace ibmw::cli::service {
namespace {
int RunCall(const Invocation& invocation) {
  auto options = ParseCallOptions(invocation);
  auto types = LoadCallTypes(options);
  const auto text = ParseCallRequest(options.request);
  const std::string dds_service_name(ibmw::rpc::GetFuncNameWithoutPrefix(types.function));
  if (options.backend == "dds" && invocation.HasOption("--ros2-compatible"))
    types.custom_service = dds_service_name.c_str();
  const auto result = ExecuteCall(std::move(options), std::move(types), text);
  if (result.exit_code == 0) std::cout << result.response << '\n';
  return result.exit_code;
}
}  // namespace
void AddCallCommand(Command& service) {
  auto& call = service.AddSubcommand("call", "Call a service once with JSON/YAML request contents");
  call.SetHandler(RunCall);
  AddCallOptions(call);
}
}  // namespace ibmw::cli::service
