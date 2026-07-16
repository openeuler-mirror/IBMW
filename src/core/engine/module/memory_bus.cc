// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/module/memory_bus.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::allocator::MemoryBus::Options> {
  using Options = ibmw::runtime::core::allocator::MemoryBus::Options;

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

namespace ibmw::runtime::core::allocator {

void MemoryBus::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "MemoryBus bus can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  options_node = options_;

  MW_INFO("[Memory] initialized");
}

void MemoryBus::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  MW_INFO("[Memory] started");
}

void MemoryBus::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  MW_INFO("[Memory] stopped");
}

const AllocatorAccess& MemoryBus::GetAllocatorAccess(const util::ModuleDetailInfo& module_info) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  return allocator_access_;
}

std::list<std::pair<std::string, std::string>> MemoryBus::GenInitializationReport() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  return {};
}

}  // namespace ibmw::runtime::core::allocator
