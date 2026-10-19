// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cstdint>
#include <stdexcept>

#include "extension_api/ibmw_extension_main.h"
#if defined(IBMW_EXT_FIXTURE_RUNTIME_ABI)
#include "extension_api/ibmw_extension_runtime.h"
#endif

namespace {

uint32_t g_create_count = 0U;
uint32_t g_engine_init_count = 0U;
uint32_t g_runtime_init_count = 0U;

class FixtureExtension final : public ibmw::EngineExtensionBase {
 public:
  std::string_view Name() const noexcept override { return "extension-v2-fixture"; }
  bool Initialize(ibmw::runtime::core::Engine*) noexcept override {
    ++g_engine_init_count;
    return true;
  }
#if defined(IBMW_EXT_FIXTURE_RUNTIME_ABI)
  uint32_t RequiredRuntimeAbi() const noexcept override {
    return IBMW_EXT_FIXTURE_RUNTIME_ABI;
  }
  bool InitializeRuntime(ibmw::extension::RuntimeV2* runtime) noexcept override {
    ++g_runtime_init_count;
    return runtime && runtime->AbiVersion() == 2;
  }
#endif
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

IBMW_CORE_EXTENSION_EXPORT uint32_t IbmwExtFixtureEngineInitCount() {
  return g_engine_init_count;
}

IBMW_CORE_EXTENSION_EXPORT uint32_t IbmwExtFixtureRuntimeInitCount() {
  return g_runtime_init_count;
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
