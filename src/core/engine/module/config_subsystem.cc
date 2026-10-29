// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/module/config_subsystem.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "engine/yaml_tools.h"
#include "base/string_util.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::configurator::ConfigSubsystem::Options> {
  using Options = ibmw::runtime::core::configurator::ConfigSubsystem::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["temp_cfg_path"] = rhs.temp_cfg_path.string();

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["temp_cfg_path"])
      rhs.temp_cfg_path = node["temp_cfg_path"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::configurator {

namespace {

bool IsSafeFileNameComponent(std::string_view name) {
  if (name.empty() || name == "." || name == "..") return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
  });
}

// Recreates `path` as a regular file readable only by the owner; returns 0 or
// an errno. The old entry is unlinked first (unlink never follows symlinks) and
// O_EXCL | O_NOFOLLOW refuses anything planted in between, so a symlink cannot
// redirect the write.
int WritePrivateFile(const std::filesystem::path& path, const std::string& content) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) return errno;
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return errno;
  size_t written = 0;
  while (written < content.size()) {
    const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      const int err = (n < 0) ? errno : EIO;
      ::close(fd);
      return err;
    }
    written += static_cast<size_t>(n);
  }
  return ::close(fd) == 0 ? 0 : errno;
}

}  // namespace

void ConfigSubsystem::Initialize(
    const std::filesystem::path& cfg_file_path) {
  MW_REQUIRE(state_ == State::kPreInit, "Config service can only be initialized once.");
  cfg_file_path_ = cfg_file_path;
  YAML::Node root;
  if (!cfg_file_path_.empty()) {
    cfg_file_path_ = std::filesystem::canonical(std::filesystem::absolute(cfg_file_path_));
    MW_INFO("Cfg file path: {}", cfg_file_path_.string());
    default_cfg_access_.SetConfigFilePath(cfg_file_path_.string());
    std::ifstream file_stream(cfg_file_path_);
    MW_REQUIRE(file_stream, "Can not open cfg file '{}'.", cfg_file_path_.string());
    std::stringstream file_data;
    file_data << file_stream.rdbuf();
    root = YAML::Load(ibmw::common::util::ReplaceEnvVars(file_data.str()));
  }
  InitializeRoot(std::move(root));
}

void ConfigSubsystem::Initialize(YAML::Node root) {
  MW_REQUIRE(root.IsMap(), "In-memory configuration must be a map.");
  InitializeRoot(YAML::Clone(root));
}

void ConfigSubsystem::InitializeRoot(YAML::Node root) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Config service can only be initialized once.");

  // Heap-allocated to avoid yaml-cpp static destruction order issues with extensions.
  // Freed in ~ConfigSubsystem().
  ori_root_options_node_ptr_ = new YAML::Node();
  root_options_node_ptr_ = new YAML::Node();
  user_root_options_node_ptr_ = new YAML::Node();

  auto& ori_root_options_node = *ori_root_options_node_ptr_;
  auto& root_options_node = *root_options_node_ptr_;
  auto& user_root_options_node = *user_root_options_node_ptr_;

  ori_root_options_node = std::move(root);
  user_root_options_node = YAML::Clone(ori_root_options_node);

  if (!ori_root_options_node["ibmw"]) {
    ori_root_options_node["ibmw"] = YAML::Node();
  }

  root_options_node["ibmw"] = YAML::Node();

  YAML::Node config_options_node = GetIbmwOptionsNode("config");
  if (config_options_node && !config_options_node.IsNull())
    options_ = config_options_node.as<Options>();

  if (!(std::filesystem::exists(options_.temp_cfg_path) &&
        std::filesystem::is_directory(options_.temp_cfg_path))) {
    std::filesystem::create_directories(options_.temp_cfg_path);
  }

  config_options_node = options_;

  MW_INFO("[Config] initialized");
}

void ConfigSubsystem::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  MW_INFO("[Config] started");
}

void ConfigSubsystem::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  MW_INFO("[Config] stopped");

  cfg_access_map_.clear();
}

YAML::Node ConfigSubsystem::GetOriRootOptionsNode() const {
  return *ori_root_options_node_ptr_;
}

YAML::Node ConfigSubsystem::GetRootOptionsNode() const {
  return *root_options_node_ptr_;
}

YAML::Node ConfigSubsystem::GetUserRootOptionsNode() const {
  return *user_root_options_node_ptr_;
}

const ConfiguratorAccess& ConfigSubsystem::GetConfiguratorAccess(
    const util::ModuleDetailInfo& module_info) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  MW_TRACE("Get configurator access for module '{}'.", module_info.module_name);

  auto itr = cfg_access_map_.find(module_info.module_name);
  if (itr != cfg_access_map_.end()) return *(itr->second);

  // If the configuration file path is specified directly, use it
  if (!module_info.cfg_file_path.empty()) {
    auto emplace_ret = cfg_access_map_.emplace(
        module_info.module_name,
        std::make_unique<ConfiguratorAccess>(module_info.cfg_file_path));
    return *(emplace_ret.first->second);
  }

  auto& ori_root_options_node = *ori_root_options_node_ptr_;
  auto& root_options_node = *root_options_node_ptr_;

  // If there is this module node in the root configuration file, the content will be generated into the temporary configuration file
  if (ori_root_options_node[module_info.module_name] &&
      !ori_root_options_node[module_info.module_name].IsNull()) {
    root_options_node[module_info.module_name] =
        ori_root_options_node[module_info.module_name];

    // module_name comes from the plugin; it must not escape temp_cfg_path.
    MW_REQUIRE(IsSafeFileNameComponent(module_info.module_name),
               "Invalid module name '{}' for temp cfg file, only [A-Za-z0-9_.-] is allowed.",
               module_info.module_name);

    std::filesystem::path temp_cfg_file_path =
        options_.temp_cfg_path /
        ("temp_cfg_file_for_" + module_info.module_name + ".yaml");

    std::stringstream ss;
    ss << root_options_node[module_info.module_name];
    const int err = WritePrivateFile(temp_cfg_file_path, ss.str());
    MW_REQUIRE(err == 0, "Can not write temp cfg file '{}': {}.",
               temp_cfg_file_path.string(), std::strerror(err));

    auto emplace_ret = cfg_access_map_.emplace(
        module_info.module_name,
        std::make_unique<ConfiguratorAccess>(temp_cfg_file_path.string()));
    return *(emplace_ret.first->second);
  }

  if (default_cfg_access_.NativeHandle()->config_file_path(default_cfg_access_.NativeHandle()->ctx).len == 0 &&
      !cfg_file_path_.empty()) {
      default_cfg_access_.SetConfigFilePath(cfg_file_path_.string());
  }

  return default_cfg_access_;
}

YAML::Node ConfigSubsystem::GetIbmwOptionsNode(std::string_view key) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  auto& ori_root_options_node = *ori_root_options_node_ptr_;
  auto& root_options_node = *root_options_node_ptr_;

  return root_options_node["ibmw"][key] = ori_root_options_node["ibmw"][key];
}

std::list<std::pair<std::string, std::string>> ConfigSubsystem::GenInitializationReport() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  auto check_msg = util::CheckYamlNodes(
      GetRootOptionsNode()["ibmw"],
      GetUserRootOptionsNode()["ibmw"],
      "ibmw");

  // Summarize top-level keys instead of dumping the full config
  std::string key_summary;
  auto ibmw_node = (*root_options_node_ptr_)["ibmw"];
  if (ibmw_node && ibmw_node.IsMap()) {
    std::vector<std::string> keys;
    for (auto it = ibmw_node.begin(); it != ibmw_node.end(); ++it) {
      keys.emplace_back(it->first.as<std::string>());
    }
    key_summary = "  sections: " + ibmw::common::util::JoinVec(keys, ", ") + "\n";
  } else {
    key_summary = "  (empty)\n";
  }

  std::list<std::pair<std::string, std::string>> report{
      {"Configuration", key_summary}};

  if (!check_msg.empty()) {
    report.emplace_back(
        std::pair<std::string, std::string>{"Configuration Warning", "  " + check_msg + "\n"});
  }

  return report;
}

}  // namespace ibmw::runtime::core::configurator
