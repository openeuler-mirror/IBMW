// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "echo_command.h"

#include <charconv>
#include <deque>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "echo_formatter.h"
#include "observation_command.h"

namespace ibmw::cli::topic {
namespace {

int RunTopicEchoCommandImpl(const Invocation& invocation) {
  auto options = ParseObservationOptions(invocation, "echo");
  const auto queue_size_text = OptionValue(invocation, "--queue-size", "100");
  size_t queue_size = 0;
  const auto [end, parse_error] = std::from_chars(
      queue_size_text.data(), queue_size_text.data() + queue_size_text.size(), queue_size);
  if (parse_error != std::errc{} || end != queue_size_text.data() + queue_size_text.size() ||
      queue_size == 0) {
    throw std::invalid_argument("--queue-size requires a positive integer.");
  }
  const bool raw = invocation.HasOption("--raw");
  const bool once = invocation.HasOption("--once");
  const auto format = OptionValue(invocation, "--format", "yaml");
  if (raw && (invocation.HasOption("--field") || invocation.HasOption("--format"))) {
    throw std::invalid_argument("--raw cannot be combined with --field or --format.");
  }
  EchoFormatter formatter(
      format, OptionValue(invocation, "--field"), raw);
  const auto packages = invocation.options.find("--type-support");
  if (packages != invocation.options.end()) {
    for (const auto& path : packages->second) formatter.LoadPackage(path);
  }
  formatter.LoadConfiguration(options.subscription.config_file);
  options.subscription.prepare_registry = [&](const auto& publisher) { formatter.Remember(publisher); };

  std::mutex mutex;
  std::deque<ibmw::transport::SerializedMessage> pending;
  std::exception_ptr error;
  bool accepted_once = false;
  size_t printed = 0;
  const int result = RunObservation(std::move(options),
      [&](ibmw::transport::SerializedMessage&& message) {
        std::lock_guard lock(mutex);
        if (error || (once && accepted_once)) return;
        try {
          if (pending.size() >= queue_size) {
            throw std::runtime_error("Echo output queue is full (" + std::to_string(queue_size) +
                                     " messages); output cannot keep up with reception.");
          }
          // Queued samples can outlive the observer Engine during final drain
          // or error cleanup. Release transport-owned storage while its DSO is
          // still loaded and keep a CLI-owned copy for decoding and output.
          if (message.payload) message.payload = ibmw::transport::CopySerializedPayload(*message.payload);
          pending.push_back(std::move(message));
          accepted_once = true;
        } catch (...) {
          error = std::current_exception();
        }
      }, {}, [&] {
        std::deque<ibmw::transport::SerializedMessage> batch;
        {
          std::lock_guard lock(mutex);
          if (error) std::rethrow_exception(error);
          batch.swap(pending);
        }
        for (const auto& message : batch) {
          std::cout << formatter.Format(message) << (format == "json" && !raw ? "\n" : "\n---\n");
          std::cout.flush();
          if (!std::cout) throw std::runtime_error("Cannot write echo output.");
          ++printed;
        }
        return once && printed != 0;
      });
  if (printed == 0) std::cerr << "ibmw topic echo: no messages received.\n";
  return result;
}

}  // namespace

void AddEchoCommand(Command& topic) {
  using Mode = OptionValueMode;
  auto& echo = topic.AddSubcommand("echo", "Print received topic messages");
  echo.SetHandler(RunTopicEchoCommandImpl);
  AddObservationOptions(echo);
  echo.AddOption("--once", {}, {}, "Print one message and exit");
  echo.AddOption(
      "--queue-size", {}, "messages",
      "Maximum messages waiting for output, positive integer (default: 100)",
      Mode::kRequired);
  echo.AddOption(
      "--field", {}, "path",
      "Print a dot-separated field path (numeric segments index arrays)",
      Mode::kRequired);
  echo.AddOption(
      "--format", {}, "format",
      "yaml (default) or json (one JSON value per line)", Mode::kRequired);
  echo.AddOption(
      "--type-support", {}, "path",
      "Local TypeSupport package for binary-to-text decoding; repeatable",
      Mode::kRequired);
  echo.AddOption(
      "--raw", {}, {}, "Print serialized bytes as hexadecimal without decoding");
}

}  // namespace ibmw::cli::topic
