// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "module_doctor.h"

#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "engine/runtime_graph/runtime_graph_client.h"
#include "engine/runtime_graph/runtime_registry.h"
#include "module_check.h"

namespace ibmw::cli::doctor {
namespace {
namespace graph = runtime::core::runtime_graph;

void PrintSummary(const ModuleCheckSummary& summary) {
  std::cout << "Module lifecycle checks: " << summary.ok << " OK, " << summary.warnings
            << " WARN, " << summary.skipped << " SKIP";
  if (summary.query_errors) std::cout << "; query errors: " << summary.query_errors;
  std::cout << '\n';
}
}  // namespace

void AddModuleDoctorOptions(Command& command, bool root) {
  command.AddOption("--runtime", {}, "id", "Only check module lifecycle records in this local runtime",
                    OptionValueMode::kRequired);
  if (!root) {
    command.AddOption("--registry-path", {}, "path",
        "Local registry root (default: " + extension::DefaultRuntimeRegistryPath().string() + ")",
        OptionValueMode::kRequired);
    command.AddOption("--report", {}, {}, "Print all recorded initialization/start results");
    command.AddOption("--report-failed", {}, {}, "Print only recorded initialization/start failures");
    command.AddOption("--include-warnings", {}, {}, "Return failure (1) for recorded module lifecycle failures");
  }
}

ModuleDoctorOptions PrepareModuleCheck(const Invocation& invocation) {
  for (const auto* flag : {"--runtime", "--registry-path", "--report", "--report-failed", "--include-warnings"}) {
    const auto entry = invocation.options.find(flag);
    if (entry == invocation.options.end()) continue;
    if (entry->second.size() != 1)
      throw std::invalid_argument(std::string(flag) + " must be specified once.");
    if ((entry->first == "--runtime" || entry->first == "--registry-path") && entry->second.front().empty())
      throw std::invalid_argument(std::string(flag) + " requires a nonempty value.");
  }
  if (invocation.HasOption("--report") && invocation.HasOption("--report-failed"))
    throw std::invalid_argument("--report and --report-failed are mutually exclusive.");
  return {.registry_path = invocation.OptionValue("--registry-path", extension::DefaultRuntimeRegistryPath().string()),
          .runtime = invocation.OptionValue("--runtime")};
}

ModuleCheckSummary RunModuleChecks(const ModuleDoctorOptions& options, const Invocation& invocation) {
  ModuleCheckSummary summary;
  const bool report = invocation.HasOption("--report");
  const bool failed_only = invocation.HasOption("--report-failed");
  const auto skip = [&](const std::string& runtime_id, const char* reason) {
    ++summary.skipped;
    if (failed_only) return;
    std::cout << "[SKIP] module";
    if (!runtime_id.empty()) std::cout << " runtime=" << std::quoted(runtime_id);
    std::cout << '\n' << "  " << reason << '\n';
  };
  const auto error = [&](const std::string& runtime_id, const char* reason) {
    ++summary.query_errors;
    std::cerr << "ibmw doctor module: error: ";
    if (!runtime_id.empty()) std::cerr << "runtime " << std::quoted(runtime_id) << ": ";
    std::cerr << reason << '\n';
  };

  std::vector<graph::RuntimeRegistration> runtimes;
  try {
    runtimes = graph::RuntimeRegistry(options.registry_path).InspectLocalRuntimes();
  } catch (const std::exception& failure) {
    error({}, failure.what());
    PrintSummary(summary);
    return summary;
  }
  bool found = false;
  for (const auto& runtime : runtimes) {
    if (!options.runtime.empty() && runtime.runtime_id != options.runtime) continue;
    found = true;
    try {
      const auto snapshot = graph::RuntimeGraphAgentClient(runtime.control_socket).ListModuleLifecycle();
      // Validate the complete response before printing any successful result.
      const auto results = CheckModuleLifecycle(snapshot);
      if (results.empty()) skip(runtime.runtime_id, "No module lifecycle records were returned.");
      for (const auto& result : results) {
        const char* status = "SKIP";
        if (result.status == CheckStatus::kOk) { ++summary.ok; status = "OK"; }
        else if (result.status == CheckStatus::kWarning) { ++summary.warnings; status = "WARN"; }
        else ++summary.skipped;
        if (!report && result.status != CheckStatus::kWarning &&
            (result.status != CheckStatus::kSkipped || failed_only)) continue;
        std::cout << '[' << status << "] module runtime=" << std::quoted(runtime.runtime_id)
                  << " pid=" << runtime.pid << " module=" << std::quoted(result.module)
                  << " kind=" << result.kind << " phase=" << result.phase;
        if (report) std::cout << " updated_at_unix_ms=" << result.updated_at_unix_ms;
        std::cout << '\n' << "  " << result.message << '\n';
      }
    } catch (const std::exception& failure) {
      error(runtime.runtime_id, failure.what());
    }
  }
  if (!found) {
    if (options.runtime.empty()) skip({}, "No live local runtimes were found in the selected registry.");
    else error(options.runtime, "Runtime was not found in the selected local registry.");
  }
  if (report) {
    std::cout << "Module scope: recorded initialization/start outcomes only, not current business health. "
                 "Records are in memory; queries require a serving Runtime Agent.\n";
  }
  PrintSummary(summary);
  return summary;
}

int RunModuleDoctor(const Invocation& invocation) {
  const auto summary = RunModuleChecks(PrepareModuleCheck(invocation), invocation);
  if (summary.query_errors) return 2;
  return summary.warnings && invocation.HasOption("--include-warnings") ? 1 : 0;
}
}  // namespace ibmw::cli::doctor
