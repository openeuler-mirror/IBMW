// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Module package used by package_loader_test. The default build reports
// three modules, the last of which answers with another name. With
// IBMW_PKG_FIXTURE_TOO_MANY it reports one module more than the loader
// accepts.

#include <cstddef>
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

IBMW_PKG_EXPORT ibmw_pkg_entry_t IbmwPkgEntry() {
  return ibmw_pkg_entry_t{
      .get_module_num = [] { return kModuleNum; },
      .get_module_name_list = &NameList,
      .create_module = &Create,
      .destroy_module = &Destroy,
      .set_driver_slot = nullptr};
}

}  // extern "C"
