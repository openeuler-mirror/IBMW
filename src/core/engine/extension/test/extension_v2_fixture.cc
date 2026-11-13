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
uint32_t g_destroy_count = 0U;

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
  ++g_destroy_count;
  delete extension;
#endif
}

}  // namespace

extern "C" {

IBMW_CORE_EXTENSION_EXPORT uint32_t IbmwExtFixtureCreateCount() {
  return g_create_count;
}

IBMW_CORE_EXTENSION_EXPORT uint32_t IbmwExtFixtureDestroyCount() {
  return g_destroy_count;
}

IBMW_CORE_EXTENSION_EXPORT uint32_t IbmwExtFixtureEngineInitCount() {
  return g_engine_init_count;
}

IBMW_CORE_EXTENSION_EXPORT uint32_t IbmwExtFixtureRuntimeInitCount() {
  return g_runtime_init_count;
}

#if defined(IBMW_EXT_FIXTURE_LEGACY)
// Entry exported by extensions built before the versioned ABI.
struct LegacyEntry {
  ibmw::EngineExtensionBase* (*create)();
  void (*destroy)(const ibmw::EngineExtensionBase*);
};
IBMW_CORE_EXTENSION_EXPORT LegacyEntry IbmwExtEntry() {
  return LegacyEntry{.create = &Create, .destroy = &Destroy};
}
#elif defined(IBMW_EXT_FIXTURE_ENTRY_V2)
// Entry exported by extensions built against extension ABI 2.
struct EntryV2 {
  uint32_t abi_version;
  uint32_t struct_size;
  ibmw::EngineExtensionBase* (*create)();
  void (*destroy)(const ibmw::EngineExtensionBase*);
};
IBMW_CORE_EXTENSION_EXPORT EntryV2 IbmwExtEntryV2() {
  return EntryV2{.abi_version = 2U, .struct_size = sizeof(EntryV2), .create = &Create, .destroy = &Destroy};
}
#else
IBMW_CORE_EXTENSION_EXPORT ibmw_ext_entry_v3_t IbmwExtEntryV3() {
  return ibmw_ext_entry_v3_t{
#if defined(IBMW_EXT_FIXTURE_BAD_VERSION)
      .abi_version = kIbmwExtensionAbiVersionV3 + 1U,
#else
      .abi_version = kIbmwExtensionAbiVersionV3,
#endif
#if defined(IBMW_EXT_FIXTURE_BAD_SIZE)
      .struct_size = sizeof(ibmw_ext_entry_v3_t) - 1U,
#else
      .struct_size = sizeof(ibmw_ext_entry_v3_t),
#endif
      .create = &Create,
      .destroy = &Destroy};
}
#endif

}  // extern "C"
