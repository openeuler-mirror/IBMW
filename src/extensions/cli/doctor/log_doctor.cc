// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "log_doctor.h"

#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>

#include "call_options.h"
#include "cpp/service/rpc_handle.h"
#include "cpp/util/type_support.h"
#include "log_client.h"
#include "log_control_ext/log_control_types.h"
#include "log_control_ext/log_control_type_support.h"

namespace ibmw::cli::doctor {
namespace {
namespace protocol = ibmw::protocols::log_control_extension;

const char* StatusName(CheckStatus status) {
  switch (status) {
    case CheckStatus::kOk: return "OK";
    case CheckStatus::kWarning: return "WARN";
    case CheckStatus::kUnknown: return "UNKNOWN";
    case CheckStatus::kSkipped: return "SKIP";
  }
  return "UNKNOWN";
}
}  // namespace

void AddLogDoctorOptions(Command& command, bool root) {
  using Mode = OptionValueMode;
  if (!root) {
    service::AddCallTransportOptions(command, false);
  } else {
    // DDS/Zenoh/domain/timeout flags already belong to topic discovery here.
    // Validation and connection setup still use the original RPC option parser.
    for (const auto* backend : {"http", "grpc", "mqtt"})
      command.AddOption(std::string("--") + backend, {}, "endpoint",
                        std::string("Log service RPC through ") + backend, Mode::kRequired);
    for (const auto& [flag, help] : {
        std::pair{"--transport-library", "Log RPC transport extension override"},
        {"--serialization", "Log RPC wire serialization"},
        {"--discovery-timeout", "Log client readiness timeout in seconds (default: 2)"},
        {"--mqtt-client-id", "Log RPC MQTT client identifier"},
        {"--mqtt-server-id", "Log RPC MQTT server identifier"},
        {"--mqtt-qos", "Log RPC MQTT QoS, 0..2 (default: 2)"},
        {"--mqtt-truststore", "MQTT TLS CA certificate"},
        {"--mqtt-client-cert", "MQTT TLS client certificate"},
        {"--mqtt-client-key", "MQTT TLS client private key"}})
      command.AddOption(flag, {}, "value", help, Mode::kRequired);
  }
  command.AddOption("--log-backend", {}, "backend", "Choose one selected backend for log RPC: dds/http/grpc/mqtt/zenoh", Mode::kRequired);
  command.AddOption("--service", {}, "name", "Log service base name (default: " + std::string(protocol::kServiceName) + ")", Mode::kRequired);
  command.AddOption("--module", {}, "name", "Check one registered logger/module (default: all in the log service)", Mode::kRequired);
  command.AddOption("--expect-level", {}, "module=level", "Explicit expected log level; repeat for different modules", Mode::kRequired);
  if (!root) {
    command.AddOption("--report", {}, {}, "Print all log check results");
    command.AddOption("--report-failed", {}, {}, "Print only log warnings and incomplete checks");
    command.AddOption("--include-warnings", {}, {}, "Return failure (1) for log level deviations");
  }
}

LogCheckOptions PrepareLogCheck(const Invocation& invocation, bool root) {
  for (const auto& [name, values] : invocation.options)
    if (name != "--expect-level" && values.size() != 1)
      throw std::invalid_argument(name + " must be specified once.");
  if (invocation.HasOption("--report") && invocation.HasOption("--report-failed"))
    throw std::invalid_argument("--report and --report-failed are mutually exclusive.");
  LogCheckOptions result;
  if (invocation.HasOption("--module")) {
    const auto& name = invocation.options.at("--module").front();
    if (name.empty()) throw std::invalid_argument("--module must not be empty.");
    result.modules.push_back(name);
  }
  if (const auto it = invocation.options.find("--expect-level"); it != invocation.options.end()) {
    for (const auto& item : it->second) {
      const auto separator = item.rfind('=');
      if (separator == std::string::npos || separator == 0)
        throw std::invalid_argument("--expect-level requires module=level.");
      const auto name = item.substr(0, separator);
      const auto level = protocol::NormalizeLogLevel(item.substr(separator + 1));
      if (!level) throw std::invalid_argument("Expected level must be TRACE, DEBUG, INFO, WARN, ERROR, FATAL or OFF.");
      if (!result.modules.empty() && name != result.modules.front())
        throw std::invalid_argument("--expect-level names a logger outside --module.");
      if (!result.expected.emplace(name, std::string(*level)).second)
        throw std::invalid_argument("Duplicate --expect-level for logger " + name + ".");
    }
  }
  result.service = invocation.HasOption("--service") ? invocation.options.at("--service").front() : protocol::kServiceName;
  if (result.service.empty() || result.service.find_first_of(": \t\r\n?#") != std::string::npos)
    throw std::invalid_argument("--service requires a service base name, without a type prefix or method.");
  std::set<std::string> selected;
  for (const auto* backend : {"dds", "http", "grpc", "mqtt", "zenoh"})
    if (invocation.HasOption(std::string("--") + backend)) selected.insert(backend);
  if (root && invocation.HasOption("--all")) { selected.insert("dds"); selected.insert("zenoh"); }
  if (invocation.HasOption("--log-backend")) {
    result.backend = invocation.options.at("--log-backend").front();
    if (!selected.contains(result.backend))
      throw std::invalid_argument("--log-backend must name a selected DDS/HTTP/gRPC/MQTT/Zenoh backend.");
  } else if (selected.size() == 1) {
    result.backend = *selected.begin();
  } else {
    result.skip_reason = selected.empty()
        ? "No log service connection selected; provide --dds, --http, --grpc, --mqtt or --zenoh. Registry and ROS 2 discovery do not locate this RPC service."
        : "Multiple log RPC backends selected; use --log-backend to choose one. No log request was sent.";
    return result;
  }
  auto& connection = result.connection;
  const auto copy = [&](const char* flag) {
    if (const auto it = invocation.options.find(flag); it != invocation.options.end()) connection.options.emplace(*it);
  };
  const auto backend_flag = "--" + result.backend;
  if (invocation.HasOption(backend_flag)) connection.options[backend_flag] = invocation.options.at(backend_flag);
  else connection.options[backend_flag] = {""};  // --all plus explicit --log-backend.
  for (const auto* flag : {"--service", "--timeout", "--discovery-timeout", "--transport-library", "--serialization",
                          "--mqtt-client-id", "--mqtt-server-id", "--mqtt-qos", "--mqtt-truststore", "--mqtt-client-cert", "--mqtt-client-key"}) copy(flag);
  if (!root || result.backend == "dds") { copy("--domain"); copy("--dds-domain"); }
  if (!root || result.backend == "zenoh") { copy("--zenoh-domain"); copy("--zenoh-config"); }
  // Preflight with the same parser before any topic discovery or RPC starts.
  auto rpc = connection;
  rpc.options.erase("--service");
  rpc.positional_args = {ibmw::rpc::GetFullFuncName("json", result.service, "GetModuleLogLevel"),
      std::string(protocol::GetModuleLogLevelReq::kTypeName) + " -> " + protocol::GetModuleLogLevelRsp::kTypeName, "{}"};
  const auto parsed = service::ParseCallOptions(rpc);
  if (!parsed.serialization.empty()) {
    using ibmw::extensions::log_control_extension::GetLogControlTypeSupport;
    const ibmw::util::TypeSupportRef request(GetLogControlTypeSupport<protocol::GetModuleLogLevelReq>());
    const ibmw::util::TypeSupportRef response(GetLogControlTypeSupport<protocol::GetModuleLogLevelRsp>());
    if (!request.CheckSerializationTypeSupported(parsed.serialization) || !response.CheckSerializationTypeSupported(parsed.serialization))
      throw std::invalid_argument("The selected serialization is not supported by the log protocol.");
  }
  if (result.backend == "dds") result.target = "domain=" + std::to_string(parsed.root["ibmw"]["extensions"]["items"][0]["options"]["domain_id"].as<uint32_t>());
  else if (result.backend == "zenoh") result.target = "domain=" + connection.OptionValue("--zenoh-domain");
  else result.target = connection.OptionValue(backend_flag);
  return result;
}

LogCheckSummary RunLogChecks(const LogCheckOptions& options, const Invocation& invocation) {
  LogCheckSummary summary;
  std::vector<LogCheckResult> results;
  if (!options.skip_reason.empty()) {
    results.push_back({.message = options.skip_reason});
  } else {
    try {
      const auto actual = log::QueryLogLevels(options.connection, options.modules, "ibmw doctor");
      results = CheckLogLevels(actual, options.expected, options.modules);
    } catch (const log::CallFailure& error) {
      if (error.exit_code == 130 || error.exit_code == 143) summary.interrupted = error.exit_code;
      results.push_back({.status = CheckStatus::kUnknown,
          .message = "Log level query failed (RPC exit code " + std::to_string(error.exit_code) + "); actual levels were not obtained."});
    } catch (const std::exception& error) {
      results.push_back({.status = CheckStatus::kUnknown, .message = "Log level query could not be completed: " + std::string(error.what())});
    }
  }
  for (const auto& result : results) {
    switch (result.status) {
      case CheckStatus::kOk: ++summary.ok; break;
      case CheckStatus::kWarning: ++summary.warnings; break;
      case CheckStatus::kUnknown: ++summary.unknown; break;
      case CheckStatus::kSkipped: ++summary.skipped; break;
    }
    const bool show = invocation.HasOption("--report") || result.status == CheckStatus::kWarning ||
        result.status == CheckStatus::kUnknown || (result.status == CheckStatus::kSkipped && !invocation.HasOption("--report-failed"));
    if (!show) continue;
    std::cout << '[' << StatusName(result.status) << "] log_level";
    if (!options.backend.empty()) std::cout << " source=" << options.backend << " target=" << std::quoted(options.target);
    std::cout << " service=" << std::quoted(options.service);
    if (!result.module.empty()) std::cout << " module=" << std::quoted(result.module);
    if (!result.actual.empty()) std::cout << " actual=" << std::quoted(result.actual);
    if (!result.expected.empty()) std::cout << " expected=" << std::quoted(result.expected) << " baseline=command_line";
    std::cout << '\n' << "  " << result.message << '\n';
  }
  std::cout << "Log level checks: " << summary.ok << " OK, " << summary.warnings << " WARN, "
            << summary.unknown << " UNKNOWN, " << summary.skipped << " SKIP\n";
  return summary;
}

int RunLogDoctor(const Invocation& invocation) {
  const auto summary = RunLogChecks(PrepareLogCheck(invocation, false), invocation);
  if (summary.interrupted) return summary.interrupted;
  if (summary.unknown) return 2;
  return summary.warnings && invocation.HasOption("--include-warnings") ? 1 : 0;
}
}  // namespace ibmw::cli::doctor
