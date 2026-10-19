// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ibmw::cli {

enum class OptionValueMode {
  kNone,
  kRequired,
  kOptional,
};

struct Invocation {
  std::vector<std::string> raw_args;
  std::vector<std::string> command_path;
  std::vector<std::string> positional_args;
  std::unordered_map<std::string, std::vector<std::string>> options;

  bool HasOption(std::string_view name) const;
  std::string OptionValue(
      std::string_view name, std::string default_value = {}) const;
};

using CommandHandler = std::function<int(const Invocation&)>;

class Command {
 public:
  bool HasSubcommand(std::string_view name) const {
    return FindSubcommand(name) != nullptr;
  }
  Command(std::string name, std::string description);
  ~Command();

  Command(const Command&) = delete;
  Command& operator=(const Command&) = delete;

  Command& AddSubcommand(std::string name, std::string description);
  Command& AddAlias(std::string alias);
  Command& AddArgument(
      std::string syntax,
      std::string description,
      bool required = true,
      bool variadic = false);
  Command& AddOption(
      std::string long_name,
      std::string short_name,
      std::string metavar,
      std::string description,
      OptionValueMode value_mode = OptionValueMode::kNone);

  // Adds an option line for a pass-through command whose detailed parser lives
  // in another component, such as a CLI provider.
  Command& AddHelpOption(std::string display, std::string description);

  Command& SetHandler(CommandHandler handler);
  Command& SetPassthrough(bool passthrough);

  int Run(int argc, char** argv) const;
  void PrintHelp(std::ostream& output, std::string invocation_path) const;

 private:
  struct ArgumentSpec {
    std::string syntax;
    std::string description;
    bool required = true;
    bool variadic = false;
  };

  struct OptionSpec {
    std::string canonical_name;
    std::string short_name;
    std::string display;
    std::string description;
    OptionValueMode value_mode = OptionValueMode::kNone;
    bool parser_visible = true;
  };

  Command* FindSubcommand(std::string_view token) const;
  const OptionSpec* FindOption(std::string_view token) const;
  int Dispatch(
      const std::vector<std::string>& raw_args,
      size_t argument_index,
      std::vector<std::string> command_path) const;
  int Invoke(
      const std::vector<std::string>& raw_args,
      size_t argument_index,
      std::vector<std::string> command_path) const;
  bool ParseInvocation(
      const std::vector<std::string>& raw_args,
      size_t argument_index,
      const std::vector<std::string>& command_path,
      Invocation& invocation,
      std::string& error) const;
  bool ContainsHelpRequest(
      const std::vector<std::string>& raw_args,
      size_t argument_index) const;
  void PrintError(std::string_view message) const;

  std::string name_;
  std::string description_;
  std::vector<std::string> aliases_;
  std::vector<std::unique_ptr<Command>> subcommands_;
  std::vector<ArgumentSpec> arguments_;
  std::vector<OptionSpec> options_;
  CommandHandler handler_;
  bool passthrough_ = false;
};

}  // namespace ibmw::cli
