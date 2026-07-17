// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/module/parameter_store.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::parameter::ParameterStore::Options> {
  using Options = ibmw::runtime::core::parameter::ParameterStore::Options;

  static Node encode(const Options& rhs) {
    Node node;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::parameter {

void ParameterStore::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "ParameterStore bus can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  options_node = options_;

  MW_INFO("[Parameters] initialized");
}

void ParameterStore::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  MW_INFO("[Parameters] started");
}

void ParameterStore::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  MW_INFO("[Parameters] stopped");

  parameter_handle_access_wrap_map_.clear();
}

const ParameterHandleAccess& ParameterStore::GetParamsAccess(
    const util::ModuleDetailInfo& module_info) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  auto itr = parameter_handle_access_wrap_map_.find(module_info.module_name);
  if (itr != parameter_handle_access_wrap_map_.end()) return itr->second->parameter_handle_access;

  auto ptr = std::make_unique<ParameterHandleAccessWrap>();
  ptr->parameter_handle.SetLogger(logger_ptr_);

  auto emplace_ret = parameter_handle_access_wrap_map_.emplace(
      module_info.module_name,
      std::move(ptr));

  return emplace_ret.first->second->parameter_handle_access;
}

ParameterHandle* ParameterStore::GetParams(std::string_view module_name) const {
  MW_REQUIRE(
      state_.load() == State::kStart,
      "Method can only be called when state is 'Start'.");

  auto itr = parameter_handle_access_wrap_map_.find(module_name);
  if (itr != parameter_handle_access_wrap_map_.end()) return &(itr->second->parameter_handle);

  return nullptr;
}

std::list<std::pair<std::string, std::string>> ParameterStore::GenInitializationReport() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  return {};
}

}  // namespace ibmw::runtime::core::parameter
