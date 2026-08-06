// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstdint>
#include <stdexcept>

#include "extension_api/ibmw_extension_main.h"

namespace {

uint32_t g_create_count = 0U;

class FixtureExtension final : public ibmw::EngineExtensionBase {
 public:
  std::string_view Name() const noexcept override { return "extension-v2-fixture"; }
  bool Initialize(ibmw::runtime::core::Engine*) noexcept override { return true; }
  void Shutdown() noexcept override {}
};

ibmw::EngineExtensionBase* Create() {
  ++g_create_count;
  return new FixtureExtension();
}

void Destroy(const ibmw::EngineExtensionBase* extension) {
#if defined(IBMW_EXT_FIXTURE_DESTROY_THROWS)
  (void)extension;
  throw std::runtime_error("fixture destroy failed");
#else
  delete extension;
#endif
}

}  // namespace

extern "C" {

IBMW_CORE_EXTENSION_EXPORT uint32_t IbmwExtFixtureCreateCount() {
  return g_create_count;
}

#if defined(IBMW_EXT_FIXTURE_LEGACY)
IBMW_CORE_EXTENSION_EXPORT ibmw_ext_entry_t IbmwExtEntry() {
  return ibmw_ext_entry_t{.create = &Create, .destroy = &Destroy};
}
#else
IBMW_CORE_EXTENSION_EXPORT ibmw_ext_entry_v2_t IbmwExtEntryV2() {
  return ibmw_ext_entry_v2_t{
#if defined(IBMW_EXT_FIXTURE_BAD_VERSION)
      .abi_version = kIbmwExtensionAbiVersionV2 + 1U,
#else
      .abi_version = kIbmwExtensionAbiVersionV2,
#endif
#if defined(IBMW_EXT_FIXTURE_BAD_SIZE)
      .struct_size = sizeof(ibmw_ext_entry_v2_t) - 1U,
#else
      .struct_size = sizeof(ibmw_ext_entry_v2_t),
#endif
      .create = &Create,
      .destroy = &Destroy};
}
#endif

}  // extern "C"
