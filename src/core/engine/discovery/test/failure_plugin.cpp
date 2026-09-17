// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstdlib>
#include <stdexcept>
#include <string_view>

#include "extension_api/ibmw_discovery_provider.h"

namespace {
using namespace ibmw::discovery;

class PluginError final : public std::exception {
 public:
  const char* what() const noexcept override { return "plugin-owned diagnostic"; }
};

void FailAt(std::string_view stage) {
  const char* setting = std::getenv("IBMW_DISCOVERY_TEST_FAILURE");
  const std::string_view failure = setting ? setting : "";
  if (!failure.starts_with(stage)) return;
  if (failure.ends_with("empty")) throw std::runtime_error("");
  if (failure.ends_with("nonstd")) throw 42;
  throw PluginError();
}

class Provider final : public DiscoveryProvider {
 public:
  void Start(const DiscoveryOptions&) override { FailAt("start"); }
  DiscoverySnapshot Query(std::chrono::steady_clock::time_point) override {
    FailAt("query");
    return {};
  }
  void Stop() noexcept override {}
};
}  // namespace

extern "C" IBMW_CORE_EXTENSION_EXPORT
ibmw::discovery::DiscoveryPluginV1 IbmwDiscoveryEntryV1() {
  FailAt("entry");
  return {.backend = ibmw::discovery::DiscoveryBackend::kDds,
          .create = []() -> ibmw::discovery::DiscoveryProvider* {
            FailAt("create");
            return new Provider;
          },
          .destroy = [](ibmw::discovery::DiscoveryProvider* provider) noexcept { delete provider; }};
}
