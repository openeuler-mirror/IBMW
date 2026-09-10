// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/extension/extension_bus.h"

#include "base/string_util.h"
#include "engine/extension/extension_runtime.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::extension::ExtensionBus::Options> {
  using Options = ibmw::runtime::core::extension::ExtensionBus::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["items"] = YAML::Node();
    for (const auto& extension_options : rhs.extensions_options) {
      Node extension_options_node;
      extension_options_node["name"] = extension_options.name;
      extension_options_node["path"] = extension_options.path;
      extension_options_node["options"] = extension_options.options;
      node["items"].push_back(extension_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["items"] && node["items"].IsSequence()) {
      for (const auto& extension_options_node : node["items"]) {
        auto extension_options = Options::ExtensionOptions{
            .name = extension_options_node["name"].as<std::string>(),
            .path = extension_options_node["path"].as<std::string>()};

        if (extension_options_node["options"])
          extension_options.options = extension_options_node["options"];
        else
          extension_options.options = YAML::Node(YAML::NodeType::Null);

        rhs.extensions_options.emplace_back(std::move(extension_options));
      }
    }
    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::extension {

void ExtensionBus::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      core_ptr_,
      "Ibmw core point is not set before ExtensionBus initialize.");

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Extension bus can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  auto tmp_registered_ext_vec = registered_extension_vec_;

  // Load and initialize all extensions
  for (auto& extension_options : options_.extensions_options) {
    // Check duplicate extension name
    auto finditr = std::find_if(
        options_.extensions_options.begin(), options_.extensions_options.end(),
        [&extension_options](const auto& op) {
          if (&extension_options == &op) return false;
          return op.name == extension_options.name;
        });
    MW_REQUIRE(finditr == options_.extensions_options.end(),
                            "Duplicate extension name {}", extension_options.name);

    EngineExtensionBase* extension_ptr = nullptr;

    if (!extension_options.path.empty()) {
      // Loading extensions
      auto ext_loader_ptr = std::make_unique<ExtensionLoader>();
      ext_loader_ptr->SetLogger(logger_ptr_);
      ext_loader_ptr->LoadExtension(extension_options.path);

      extension_ptr = ext_loader_ptr->GetExtension();

      auto ext_name = extension_ptr->Name();

      // Check the extension name
      MW_REQUIRE(
          ext_name == extension_options.name,
          "Require extension name '{}', but get extension name '{}' in lib {}.",
          extension_options.name, ext_name, extension_options.path);

      extension_options.path = ext_loader_ptr->GetDynamicLib().GetLibFullPath();

      extension_loader_vec_.emplace_back(std::move(ext_loader_ptr));

    } else {
      // Find in directly registered extensions
      auto finditr = std::find_if(
          tmp_registered_ext_vec.begin(), tmp_registered_ext_vec.end(),
          [&extension_options](const EngineExtensionBase* ext) {
            return ext->Name() == extension_options.name;
          });

      MW_REQUIRE(finditr != tmp_registered_ext_vec.end(),
                              "Can not find extension {}", extension_options.name);

      extension_ptr = *finditr;

      tmp_registered_ext_vec.erase(finditr);
    }

    // Initialize the extension
    bool ret = false;
    if (extension_ptr->RequiredRuntimeAbi() ==
        ibmw::extension::kRuntimeAbiVersionV1) {
      if (!runtime_v1_) {
        runtime_v1_ = MakeExtensionRuntimeV1(*core_ptr_);
      }
      ret = extension_ptr->InitializeRuntime(runtime_v1_.get());
    } else {
      ret = extension_ptr->Initialize(core_ptr_);
    }
    MW_REQUIRE(ret, "Init extension '{}' failed.", extension_options.name);

    used_extension_vec_.emplace_back(extension_ptr);
    MW_TRACE("Load extension '{}' succeeded.", extension_options.name);
  }

  if (!tmp_registered_ext_vec.empty()) {
    std::vector<std::string> unconfigured_extensions;
    unconfigured_extensions.reserve(tmp_registered_ext_vec.size());
    for (auto* itr : tmp_registered_ext_vec)
      unconfigured_extensions.emplace_back(itr->Name());
    MW_THROW("Some extensions are not configured, {}",
                      ibmw::common::util::JoinVec(unconfigured_extensions, ","));
  }

  options_node = options_;

  MW_INFO("[Extensions] initialized");
}

void ExtensionBus::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  MW_INFO("[Extensions] started");
}

void ExtensionBus::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  MW_INFO("[Extensions] stopped");

  // Execute Shutdown in reverse order
  for (auto itr = extension_loader_vec_.rbegin(); itr != extension_loader_vec_.rend(); ++itr) {
    (*itr)->GetExtension()->Shutdown();
  }

  // extension_loader_vec_ cannot be cleaned up, there are many callbacks registered from other dlls

  core_ptr_ = nullptr;
}

void ExtensionBus::RegisterExtension(EngineExtensionBase* extension) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  MW_REQUIRE(extension != nullptr, "Register invalid extension");

  registered_extension_vec_.emplace_back(extension);
}

void ExtensionBus::RegisterCorePtr(Engine* core_ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  core_ptr_ = core_ptr;
}

YAML::Node ExtensionBus::GetExtensionOptionsNode(std::string_view ext_name) const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  auto finditr = std::find_if(
      options_.extensions_options.begin(),
      options_.extensions_options.end(),
      [ext_name](const auto& op) { return ext_name == op.name; });

  if (finditr != options_.extensions_options.end())
    return finditr->options;

  return YAML::Node();
}

void ExtensionBus::UpdateExtensionOptionsNode(std::string_view ext_name, YAML::Node options) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  auto finditr = std::find_if(
      options_.extensions_options.begin(),
      options_.extensions_options.end(),
      [ext_name](const auto& op) { return ext_name == op.name; });

  if (finditr != options_.extensions_options.end())
    finditr->options = options;
}

std::list<std::pair<std::string, std::string>> ExtensionBus::GenInitializationReport() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  std::string ext_summary;
  if (options_.extensions_options.empty()) {
    ext_summary = "  (none)\n";
  } else {
    for (const auto& extension_options : options_.extensions_options) {
      ext_summary += "  " + extension_options.name + "  (" + extension_options.path + ")\n";
    }
  }

  std::list<std::pair<std::string, std::string>> report{
      {"Extensions", ext_summary}};

  for (const auto& extension_ptr : used_extension_vec_) {
    report.splice(report.end(), extension_ptr->GenInitializationReport());
  }

  return report;
}

const std::vector<EngineExtensionBase*>& ExtensionBus::GetUsedExtension() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  return used_extension_vec_;
}

}  // namespace ibmw::runtime::core::extension
