// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "doctor_command.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/command.h"
#include "discovery_options.h"
#include "engine/discovery/registry_discovery_provider.h"
#include "topic_check.h"
#include "qos_check.h"
#include "log_doctor.h"

namespace ibmw::cli::doctor {
namespace {

std::chrono::milliseconds ParseTimeout(const Invocation& invocation) {
  const auto value = invocation.OptionValue("--timeout", "2");
  double seconds = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(seconds) ||
      seconds < 0.01 || seconds > 60) {
    throw std::invalid_argument("--timeout must be between 0.01 and 60 seconds.");
  }
  return std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
}

discovery::DiscoverySnapshot CollectTopics(
    const std::vector<discovery::DiscoveryOptions>& options,
    std::chrono::milliseconds timeout) {
  discovery::DiscoverySnapshot snapshot;
  std::vector<discovery::DiscoveryOptions> network;
  for (const auto& option : options) {
    if (option.backend != discovery::DiscoveryBackend::kRegistry) {
      network.push_back(option);
      continue;
    }
    try {
      discovery::RegistryDiscoveryProvider provider;
      provider.Start(option);
      snapshot = provider.QueryReadOnly();
      provider.Stop();
    } catch (const std::exception& error) {
      snapshot.sources.push_back({.source = discovery::DescribeSource(option),
          .status = discovery::DiscoveryStatus::kError, .message = error.what()});
    }
  }
  if (!network.empty()) {
    auto remote = discovery::Discover(network, timeout);
    snapshot.sources.insert(snapshot.sources.end(),
        std::make_move_iterator(remote.sources.begin()), std::make_move_iterator(remote.sources.end()));
    snapshot.endpoints.insert(snapshot.endpoints.end(),
        std::make_move_iterator(remote.endpoints.begin()), std::make_move_iterator(remote.endpoints.end()));
  }
  return snapshot;
}

const char* StatusName(CheckStatus status) {
  switch (status) {
    case CheckStatus::kOk: return "OK";
    case CheckStatus::kWarning: return "WARN";
    case CheckStatus::kUnknown: return "UNKNOWN";
    case CheckStatus::kSkipped: return "SKIP";
  }
  return "UNKNOWN";
}

void PrintResult(const TopicCheckResult& result, bool details) {
  std::cout << '[' << StatusName(result.status) << "] topic";
  if (!result.topic.empty()) std::cout << ' ' << std::quoted(result.topic);
  std::cout << " source=" << discovery::BackendName(result.source.backend);
  if (!result.source.domain.empty()) std::cout << " domain=" << std::quoted(result.source.domain);
  if (!result.source.instance_id.empty()) std::cout << " instance=" << std::quoted(result.source.instance_id);
  if (!result.topic.empty()) {
    std::cout << " type=" << std::quoted(result.type.empty() ? "<unknown>" : result.type)
              << " publishers=" << result.publishers << " subscribers=" << result.subscribers;
  }
  std::cout << '\n' << "  " << result.message << '\n';
  if (details && result.status == CheckStatus::kWarning) {
    std::cout << "  Suggestion: check the expected subscriber, selected source/domain, and discovery window.\n";
  }
}

int Run(const Invocation& invocation) {
  for (const auto& [name, values] : invocation.options) {
    if (name != "--expect-level" && values.size() != 1) throw std::invalid_argument(name + " must be specified once.");
  }
  if (invocation.HasOption("--report") && invocation.HasOption("--report-failed")) {
    throw std::invalid_argument("--report and --report-failed are mutually exclusive.");
  }
  const auto timeout = ParseTimeout(invocation);
  const auto options = topic::ParseDiscoveryOptions(invocation);
  const auto log_options = PrepareLogCheck(invocation, true);
  const auto snapshot = CollectTopics(options, timeout);
  const auto results = CheckTopics(snapshot, invocation.HasOption("--include-hidden-topics"));
  const auto qos_results = CheckQos(snapshot, invocation.HasOption("--include-hidden-topics"));
  size_t ok = 0, warnings = 0, unknown = 0, skipped = 0;
  const bool report = invocation.HasOption("--report");
  const bool details = report || invocation.HasOption("--report-failed");
  for (const auto& result : results) {
    switch (result.status) {
      case CheckStatus::kOk: ++ok; break;
      case CheckStatus::kWarning: ++warnings; break;
      case CheckStatus::kUnknown: ++unknown; break;
      case CheckStatus::kSkipped: ++skipped; break;
    }
    if (report || result.status == CheckStatus::kWarning || result.status == CheckStatus::kUnknown) {
      PrintResult(result, details);
    }
  }
  if (!report && ok == 0 && warnings == 0 && unknown == 0) {
    std::cout << "[SKIP] topic: no visible publisher endpoints to check in the selected sources.\n";
  }
  if (details) {
    for (const auto& source : snapshot.sources) {
      if (!source.observed_at) continue;
      std::cout << "Observed source=" << discovery::BackendName(source.source.backend)
                << " domain=" << std::quoted(source.source.domain)
                << " instance=" << std::quoted(source.source.instance_id)
                << " at_unix_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
                       source.observed_at->time_since_epoch()).count() << '\n';
    }
    std::cout << "Network discovery window: " << timeout.count() << " ms (registry is a local snapshot).\n";
    std::cout << "Scope: selected sources only; topic presence and per-policy QoS checks do not prove delivery.\n";
  }
  std::cout << "Topic checks: " << ok << " OK, " << warnings << " WARN, "
            << unknown << " UNKNOWN, " << skipped << " SKIP\n";
  size_t qos_ok = 0, qos_warnings = 0, qos_unknown = 0, qos_skipped = 0;
  for (const auto& result : qos_results) {
    switch (result.status) {
      case CheckStatus::kOk: ++qos_ok; break;
      case CheckStatus::kWarning: ++qos_warnings; break;
      case CheckStatus::kUnknown: ++qos_unknown; break;
      case CheckStatus::kSkipped: ++qos_skipped; break;
    }
    if (!report && result.status != CheckStatus::kWarning && result.status != CheckStatus::kUnknown) continue;
    std::cout << '[' << StatusName(result.status) << "] qos topic=" << std::quoted(result.topic)
              << " source=" << discovery::BackendName(result.source.backend)
              << " domain=" << std::quoted(result.source.domain)
              << " instance=" << std::quoted(result.source.instance_id)
              << " driver=" << std::quoted(result.driver) << " type=" << std::quoted(result.type)
              << " policy=" << result.policy << '\n';
    if (!result.publisher.empty()) std::cout << "  publisher=" << std::quoted(result.publisher) << '\n';
    if (!result.subscriber.empty()) std::cout << "  subscriber=" << std::quoted(result.subscriber) << '\n';
    if (!result.offered.empty()) {
      if (!result.publisher.empty() && !result.subscriber.empty()) {
        std::cout << "  offered=" << std::quoted(result.offered) << " requested=" << std::quoted(result.requested) << '\n';
      } else std::cout << "  value=" << std::quoted(result.offered) << '\n';
    }
    std::cout << "  " << result.message << '\n';
  }
  std::cout << "QoS policy checks: " << qos_ok << " OK, " << qos_warnings << " WARN, "
            << qos_unknown << " UNKNOWN, " << qos_skipped << " SKIP\n";
  unknown += qos_unknown;
  warnings += qos_warnings;
  const auto logs = RunLogChecks(log_options, invocation);
  unknown += logs.unknown;
  warnings += logs.warnings;
  std::cout << "Doctor checks: " << warnings << " WARN, " << unknown << " UNKNOWN\n";
  if (logs.interrupted) return logs.interrupted;
  if (unknown != 0) return 2;
  return warnings != 0 && invocation.HasOption("--include-warnings") ? 1 : 0;
}

}  // namespace
}  // namespace ibmw::cli::doctor

int RunDoctorCommand(int argc, const char* const* argv) {
  using namespace ibmw::cli;
  try {
    Command command("ibmw doctor", "Check topics, DDS/ROS 2 QoS and runtime log levels");
    bool invoked = false;
    command.SetHandler([&invoked](const Invocation& invocation) {
      invoked = true;
      return doctor::Run(invocation);
    });
    topic::AddDiscoveryOptions(command);
    command.AddOption("--timeout", {}, "seconds", "Topic discovery window (default: 2), 0.01..60 seconds; explicit value also sets log RPC deadline",
                      OptionValueMode::kRequired);
    command.AddOption("--report", {}, {}, "Print all topic, QoS and log results, including coverage gaps");
    command.AddOption("--report-failed", {}, {}, "Print details for warnings and incomplete checks");
    command.AddOption("--include-warnings", {}, {}, "Return failure (1) when topic, QoS or log warnings are found");
    command.AddOption("--include-hidden-topics", {}, {}, "Consider hidden topics as well");
    doctor::AddLogDoctorOptions(command, true);
    auto& log = command.AddSubcommand("log", "Check only runtime logger levels through the existing log service");
    doctor::AddLogDoctorOptions(log, false);
    log.SetHandler([&invoked](const Invocation& invocation) {
      invoked = true;
      return doctor::RunLogDoctor(invocation);
    });
    // The generic dispatcher displays help for an empty command with children.
    // Doctor intentionally keeps its no-argument, run-all behavior.
    if (argc <= 1) return doctor::Run({});
    std::vector<std::string> arguments;
    for (int index = 0; index < argc; ++index) arguments.emplace_back(argv[index]);
    std::vector<char*> mutable_argv;
    for (auto& argument : arguments) mutable_argv.push_back(argument.data());
    const auto code = command.Run(static_cast<int>(mutable_argv.size()), mutable_argv.data());
    return !invoked && code != 0 ? 2 : code;
  } catch (const std::exception& error) {
    std::cerr << "ibmw doctor: error: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "ibmw doctor: error: Unknown failure.\n";
  }
  return 2;
}
