// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Module package used by package_loader_test. The default build reports
// three modules, the last of which answers with another name. With
// IBMW_PKG_FIXTURE_TOO_MANY it reports one module more than the loader
// accepts. IBMW_PKG_FIXTURE_LEGACY exports only the unversioned IbmwPkgEntry,
// IBMW_PKG_FIXTURE_BAD_VERSION and IBMW_PKG_FIXTURE_SHORT export a V2 entry
// with a wrong version or a too small struct_size.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pkg/pkg_main.h"

namespace {

#ifdef IBMW_PKG_FIXTURE_TOO_MANY
constexpr size_t kModuleNum = 4097;
#else
constexpr size_t kModuleNum = 3;
#endif

int g_live_modules = 0;

const std::vector<std::string>& Names() {
  static const std::vector<std::string> names = [] {
    std::vector<std::string> v;
    for (size_t i = 0; i < kModuleNum; ++i) v.push_back("m" + std::to_string(i));
    return v;
  }();
  return names;
}

const ibmw_string_view_t* NameList() {
  static const std::vector<ibmw_string_view_t> views = [] {
    std::vector<ibmw_string_view_t> v;
    for (const auto& name : Names()) v.push_back({name.data(), name.size()});
    return v;
  }();
  return views.data();
}

struct FixtureModule {
  ibmw_module_base_t base{};
  std::string name;
};

ibmw_module_info_t Info(void* ctx) {
  const auto* module = static_cast<FixtureModule*>(ctx);
  ibmw_module_info_t info{};
  info.module_name = {module->name.data(), module->name.size()};
  return info;
}

const ibmw_module_base_t* Create(ibmw_string_view_t name) {
  auto* module = new FixtureModule;
  module->name.assign(name.str, name.len);
  if (module->name == Names().back()) module->name = "renamed";
  module->base.info = &Info;
  module->base.ctx = module;
  ++g_live_modules;
  return &module->base;
}

void Destroy(const ibmw_module_base_t* base) {
  delete static_cast<FixtureModule*>(base->ctx);
  --g_live_modules;
}

}  // namespace

extern "C" {

IBMW_PKG_EXPORT int IbmwPkgFixtureLiveModules() { return g_live_modules; }

#ifdef IBMW_PKG_FIXTURE_LEGACY
// Layout of the unversioned package ABI.
struct LegacyPkgEntry {
  size_t (*get_module_num)();
  const ibmw_string_view_t* (*get_module_name_list)();
  const ibmw_module_base_t* (*create_module)(ibmw_string_view_t);
  void (*destroy_module)(const ibmw_module_base_t*);
  void (*set_driver_slot)(ibmw_string_view_t, ibmw_driver_slot_t, void*);
};

IBMW_PKG_EXPORT LegacyPkgEntry IbmwPkgEntry() {
  return LegacyPkgEntry{[] { return kModuleNum; }, &NameList, &Create, &Destroy, nullptr};
}
#else
IBMW_PKG_EXPORT ibmw_pkg_entry_v2_t IbmwPkgEntryV2() {
#if defined(IBMW_PKG_FIXTURE_BAD_VERSION)
  constexpr uint32_t kVersion = IBMW_PKG_ABI_VERSION_V2 + 1;
#else
  constexpr uint32_t kVersion = IBMW_PKG_ABI_VERSION_V2;
#endif
#if defined(IBMW_PKG_FIXTURE_SHORT)
  constexpr uint32_t kSize = sizeof(ibmw_pkg_entry_v2_t) - sizeof(void*);
#else
  constexpr uint32_t kSize = sizeof(ibmw_pkg_entry_v2_t);
#endif
  return ibmw_pkg_entry_v2_t{
      .abi_version = kVersion,
      .struct_size = kSize,
      .get_module_num = [] { return kModuleNum; },
      .get_module_name_list = &NameList,
      .create_module = &Create,
      .destroy_module = &Destroy,
      .set_driver_slot = nullptr};
}
#endif

}  // extern "C"
