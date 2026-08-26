// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "cli_provider_loader.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include "engine/dynamic_lib.h"
#include "extension_api/ibmw_cli_provider.h"
#include "yaml-cpp/yaml.h"

namespace {

std::filesystem::path ExecutableDirectory() {
  std::array<char, 4096> buffer{};
  const ssize_t size =
      ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (size <= 0) {
    return std::filesystem::current_path();
  }
  return std::filesystem::path(
             std::string(buffer.data(), static_cast<size_t>(size)))
      .parent_path();
}

std::filesystem::path DefaultCliConfigPath() {
  if (const char* configured = std::getenv("IBMW_CLI_CONFIG");
      configured != nullptr && configured[0] != '\0') {
    return std::filesystem::path(configured);
  }

  const auto executable_directory = ExecutableDirectory();
  const auto adjacent = executable_directory / "cli.yaml";
  if (std::filesystem::exists(adjacent)) {
    return adjacent;
  }
  const auto build_tree = executable_directory.parent_path() / "cli.yaml";
  if (std::filesystem::exists(build_tree)) {
    return build_tree;
  }

  // Installed layout: <prefix>/bin/ibmw and <prefix>/share/ibmw/cli.yaml.
  return executable_directory.parent_path() / "share" / "ibmw" / "cli.yaml";
}

std::string ReadString(ibmw_string_view_t value, std::string_view field) {
  if (value.len != 0 && value.str == nullptr) {
    throw std::runtime_error(std::string("CLI ") + std::string(field) +
                             " has a null string pointer.");
  }
  return std::string(value.str == nullptr ? "" : value.str, value.len);
}

struct LoadedCommand {
  std::string name;
  std::string description;
  ibmw_cli_command_invoke_v1_t invoke = nullptr;
};

struct LoadedProvider {
  ibmw::common::util::DynamicLib library;
  std::string path;
  std::string name;
  std::vector<LoadedCommand> commands;
};

std::filesystem::path ResolveProviderPath(
    const std::filesystem::path& config_path, std::string_view value) {
  std::filesystem::path path(value);
  if (path.is_relative()) {
    path = config_path.parent_path() / path;
  }
  return path.lexically_normal();
}

std::vector<std::unique_ptr<LoadedProvider>> LoadProviders(
    const std::filesystem::path& config_path) {
  YAML::Node root = YAML::LoadFile(config_path.string());
  if (!root.IsMap() || !root["providers"] || !root["providers"].IsSequence()) {
    throw std::runtime_error(
        "CLI configuration must contain a 'providers' sequence.");
  }

  std::vector<std::unique_ptr<LoadedProvider>> providers;
  std::set<std::string> command_names;
  for (const auto& provider_node : root["providers"]) {
    if (!provider_node.IsMap() || !provider_node["library"] ||
        !provider_node["library"].IsScalar()) {
      throw std::runtime_error(
          "Each CLI provider requires a scalar 'library' path.");
    }
    const auto library_path = ResolveProviderPath(
        config_path, provider_node["library"].as<std::string>());
    auto provider = std::make_unique<LoadedProvider>();
    provider->path = library_path.string();
    if (!provider->library.Load(provider->path)) {
      throw std::runtime_error(
          "Cannot load CLI provider '" + provider->path + "': " +
          ibmw::common::util::DynamicLib::GetErr());
    }

    using EntryFunc = ibmw_cli_entry_v1_t (*)();
    auto* entry_symbol = provider->library.GetSymbol("IbmwCliEntryV1");
    if (entry_symbol == nullptr) {
      throw std::runtime_error(
          "CLI provider '" + provider->path +
          "' does not export IbmwCliEntryV1.");
    }
    const auto entry = reinterpret_cast<EntryFunc>(entry_symbol)();
    if (entry.abi_version != kIbmwCliAbiVersionV1) {
      throw std::runtime_error(
          "CLI provider '" + provider->path + "' ABI version mismatch.");
    }
    if (entry.struct_size != sizeof(ibmw_cli_entry_v1_t)) {
      throw std::runtime_error(
          "CLI provider '" + provider->path + "' descriptor size mismatch.");
    }
    provider->name = ReadString(entry.provider_name, "provider name");
    if (provider->name.empty()) {
      throw std::runtime_error(
          "CLI provider '" + provider->path + "' has an empty provider name.");
    }
    constexpr size_t kMaxCommands = 256;
    if (entry.command_count > kMaxCommands ||
        (entry.command_count != 0 && entry.commands == nullptr)) {
      throw std::runtime_error(
          "CLI provider '" + provider->path + "' has an invalid command list.");
    }
    provider->commands.reserve(entry.command_count);
    for (size_t index = 0; index < entry.command_count; ++index) {
      const auto& command = entry.commands[index];
      LoadedCommand loaded{
          .name = ReadString(command.name, "command name"),
          .description = ReadString(command.description, "command description"),
          .invoke = command.invoke};
      if (loaded.name.empty() || loaded.invoke == nullptr) {
        throw std::runtime_error(
            "CLI provider '" + provider->path +
            "' contains an invalid command descriptor.");
      }
      if (!command_names.emplace(loaded.name).second) {
        throw std::runtime_error(
            "Duplicate CLI command '" + loaded.name + "'.");
      }
      provider->commands.emplace_back(std::move(loaded));
    }
    providers.emplace_back(std::move(provider));
  }
  return providers;
}

}  // namespace

int RunCliCommandFromProviders(
    int argc, char** argv, const std::filesystem::path& configured_path) {
  if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
    return kCliCommandNotFound;
  }

  const auto config_path = configured_path.empty()
                               ? DefaultCliConfigPath()
                               : configured_path;
  if (!std::filesystem::is_regular_file(config_path)) {
    std::cerr << "CLI configuration not found: '" << config_path.string()
              << "'.\n";
    return 1;
  }

  try {
    auto providers = LoadProviders(config_path);
    const std::string command_name = argv[0];
    for (auto& provider : providers) {
      for (const auto& command : provider->commands) {
        if (command.name == command_name) {
          return command.invoke(argc, const_cast<const char* const*>(argv));
        }
      }
    }
  } catch (const std::exception& error) {
    std::cerr << "CLI provider loading failed: " << error.what() << '\n';
    return 1;
  }
  return kCliCommandNotFound;
}
