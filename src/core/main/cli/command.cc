// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "cli/command.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <utility>

namespace ibmw::cli {

namespace {

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

std::pair<std::string_view, std::string_view> SplitOption(
    std::string_view token) {
  const auto equal = token.find('=');
  if (equal == std::string_view::npos) return {token, {}};
  return {token.substr(0, equal), token.substr(equal + 1)};
}

std::string JoinPath(const std::vector<std::string>& path) {
  std::string result;
  for (const auto& item : path) {
    if (!result.empty()) result += ' ';
    result += item;
  }
  return result;
}

}  // namespace

bool Invocation::HasOption(std::string_view name) const {
  return options.find(std::string(name)) != options.end();
}

std::string Invocation::OptionValue(
    std::string_view name, std::string default_value) const {
  const auto finditr = options.find(std::string(name));
  if (finditr == options.end() || finditr->second.empty()) {
    return default_value;
  }
  return finditr->second.back().empty() ? default_value : finditr->second.back();
}

Command::Command(std::string name, std::string description)
    : name_(std::move(name)), description_(std::move(description)) {}

Command::~Command() = default;

Command& Command::AddSubcommand(
    std::string name, std::string description) {
  subcommands_.push_back(
      std::make_unique<Command>(std::move(name), std::move(description)));
  return *subcommands_.back();
}

Command& Command::AddAlias(std::string alias) {
  aliases_.push_back(std::move(alias));
  return *this;
}

Command& Command::AddArgument(
    std::string syntax,
    std::string description,
    bool required,
    bool variadic) {
  arguments_.push_back(ArgumentSpec{
      .syntax = std::move(syntax),
      .description = std::move(description),
      .required = required,
      .variadic = variadic});
  return *this;
}

Command& Command::AddOption(
    std::string long_name,
    std::string short_name,
    std::string metavar,
    std::string description,
    OptionValueMode value_mode) {
  std::string display = long_name;
  if (!short_name.empty()) display = short_name + ", " + display;
  if (value_mode == OptionValueMode::kRequired) {
    display += " <" + metavar + ">";
  } else if (value_mode == OptionValueMode::kOptional) {
    display += "[=<" + metavar + ">]";
  }

  options_.push_back(OptionSpec{
      .canonical_name = std::move(long_name),
      .short_name = std::move(short_name),
      .display = std::move(display),
      .description = std::move(description),
      .value_mode = value_mode,
      .parser_visible = true});
  return *this;
}

Command& Command::AddHelpOption(
    std::string display, std::string description) {
  options_.push_back(OptionSpec{
      .canonical_name = {},
      .short_name = {},
      .display = std::move(display),
      .description = std::move(description),
      .value_mode = OptionValueMode::kNone,
      .parser_visible = false});
  return *this;
}

Command& Command::SetHandler(CommandHandler handler) {
  handler_ = std::move(handler);
  return *this;
}

Command& Command::SetPassthrough(bool passthrough) {
  passthrough_ = passthrough;
  return *this;
}

Command* Command::FindSubcommand(std::string_view token) const {
  for (const auto& command : subcommands_) {
    if (command->name_ == token ||
        std::find(command->aliases_.begin(), command->aliases_.end(), token) !=
            command->aliases_.end()) {
      return command.get();
    }
  }
  return nullptr;
}

const Command::OptionSpec* Command::FindOption(std::string_view token) const {
  const auto [name, value] = SplitOption(token);
  static_cast<void>(value);
  for (const auto& option : options_) {
    if (option.parser_visible &&
        (option.canonical_name == name || option.short_name == name)) {
      return &option;
    }
  }
  return nullptr;
}

bool Command::ContainsHelpRequest(
    const std::vector<std::string>& raw_args,
    size_t argument_index) const {
  return std::any_of(
      raw_args.begin() + static_cast<std::ptrdiff_t>(argument_index),
      raw_args.end(), [](const std::string& token) {
        return token == "--help" || token == "-h";
      });
}

int Command::Dispatch(
    const std::vector<std::string>& raw_args,
    size_t argument_index,
    std::vector<std::string> command_path) const {
  if (passthrough_ && handler_ && subcommands_.empty()) {
    return Invoke(raw_args, argument_index, std::move(command_path));
  }
  if (argument_index >= raw_args.size()) {
    if (!subcommands_.empty()) {
      PrintHelp(std::cout, JoinPath(command_path));
      return 0;
    }
    return Invoke(raw_args, argument_index, std::move(command_path));
  }

  if (raw_args[argument_index] == "--help" ||
      raw_args[argument_index] == "-h") {
    PrintHelp(std::cout, JoinPath(command_path));
    return 0;
  }

  if (raw_args[argument_index] == "help" && !subcommands_.empty()) {
    PrintHelp(std::cout, JoinPath(command_path));
    return 0;
  }

  if (auto* subcommand = FindSubcommand(raw_args[argument_index])) {
    command_path.push_back(subcommand->name_);
    return subcommand->Dispatch(raw_args, argument_index + 1,
                                std::move(command_path));
  }

  if (ContainsHelpRequest(raw_args, argument_index) && handler_) {
    PrintHelp(std::cout, JoinPath(command_path));
    return 0;
  }

  if (!subcommands_.empty() && !handler_) {
    PrintError("unknown subcommand '" + raw_args[argument_index] + "'");
    return 1;
  }

  return Invoke(raw_args, argument_index, std::move(command_path));
}

int Command::Invoke(
    const std::vector<std::string>& raw_args,
    size_t argument_index,
    std::vector<std::string> command_path) const {
  Invocation invocation{
      .raw_args = raw_args,
      .command_path = std::move(command_path)};

  std::string error;
  if (!passthrough_ && !ParseInvocation(
                           raw_args, argument_index, invocation.command_path,
                           invocation, error)) {
    return 1;
  }

  if (passthrough_) {
    invocation.positional_args.assign(
        raw_args.begin() + static_cast<std::ptrdiff_t>(argument_index),
        raw_args.end());
  }

  if (!handler_) {
    PrintHelp(std::cout, JoinPath(invocation.command_path));
    return 0;
  }
  return handler_(invocation);
}

bool Command::ParseInvocation(
    const std::vector<std::string>& raw_args,
    size_t argument_index,
    const std::vector<std::string>& command_path,
    Invocation& invocation,
    std::string& error) const {
  static_cast<void>(command_path);
  for (size_t index = argument_index; index < raw_args.size(); ++index) {
    const auto& token = raw_args[index];
    if (token == "--") {
      invocation.positional_args.insert(
          invocation.positional_args.end(), raw_args.begin() + index + 1,
          raw_args.end());
      break;
    }

    if (!StartsWith(token, "-")) {
      invocation.positional_args.push_back(token);
      continue;
    }

    const auto* option = FindOption(token);
    if (option == nullptr) {
      error = "unknown option '" + token + "'";
      PrintError(error);
      return false;
    }

    const auto [name, inline_value] = SplitOption(token);
    if (option->value_mode == OptionValueMode::kNone) {
      if (!inline_value.empty()) {
        error = "option '" + std::string(name) + "' does not take a value";
        PrintError(error);
        return false;
      }
      invocation.options[option->canonical_name].emplace_back();
      continue;
    }

    std::string value(inline_value);
    if (value.empty() && option->value_mode == OptionValueMode::kRequired) {
      if (index + 1 >= raw_args.size() ||
          StartsWith(raw_args[index + 1], "-")) {
        error = "option '" + std::string(name) + "' requires a value";
        PrintError(error);
        return false;
      }
      value = raw_args[++index];
    }
    invocation.options[option->canonical_name].push_back(std::move(value));
  }

  for (size_t index = 0; index < arguments_.size(); ++index) {
    if (arguments_[index].required &&
        invocation.positional_args.size() <= index) {
      error = "missing required argument '" + arguments_[index].syntax + "'";
      PrintError(error);
      return false;
    }
  }

  const bool accepts_extra_positionals =
      !arguments_.empty() && arguments_.back().variadic;
  if (!accepts_extra_positionals &&
      invocation.positional_args.size() > arguments_.size()) {
    const auto unexpected_index = arguments_.empty()
                                      ? 0
                                      : arguments_.size();
    error = "unexpected positional argument '" +
            invocation.positional_args[unexpected_index] + "'";
    PrintError(error);
    return false;
  }
  return true;
}

void Command::PrintError(std::string_view message) const {
  std::cerr << "ibmw: error: " << message << "\n";
}

void Command::PrintHelp(
    std::ostream& output, std::string invocation_path) const {
  output << "Usage: " << invocation_path;
  if (!subcommands_.empty()) output << " <command>";
  for (const auto& argument : arguments_) {
    output << ' ' << argument.syntax;
  }
  if (!options_.empty() || !subcommands_.empty()) output << " [options]";
  output << "\n\n" << description_ << "\n";

  if (!subcommands_.empty()) {
    output << "\nCommands:\n";
    for (const auto& command : subcommands_) {
    output << "  " << command->name_;
    if (!command->aliases_.empty()) {
        output << " (";
        for (size_t index = 0; index < command->aliases_.size(); ++index) {
          if (index != 0) output << ", ";
          output << command->aliases_[index];
        }
        output << ')';
    }
      output << "\t" << command->description_ << "\n";
    }
  }

  if (!arguments_.empty()) {
    output << "\nArguments:\n";
    for (const auto& argument : arguments_) {
      output << "  " << argument.syntax << "\t" << argument.description
             << "\n";
    }
  }

  if (!options_.empty() || !subcommands_.empty()) {
    output << "\nOptions:\n";
    for (const auto& option : options_) {
      output << "  " << option.display << "\t" << option.description << "\n";
    }
    output << "  -h, --help\tShow this help\n";
  }
}

int Command::Run(int argc, char** argv) const {
  std::vector<std::string> raw_args;
  raw_args.reserve(static_cast<size_t>(std::max(argc, 1)));
  for (int index = 0; index < argc; ++index) {
    raw_args.emplace_back(argv[index] == nullptr ? "" : argv[index]);
  }
  if (raw_args.empty()) raw_args.emplace_back(name_);

  std::vector<std::string> command_path{name_};
  return Dispatch(raw_args, 1, std::move(command_path));
}

}  // namespace ibmw::cli
