#include <extension_api/ibmw_reporter_v1.h>

#include <cstdlib>
#include <dlfcn.h>
#include <iostream>

#if IBMW_EXPECT_REPORTER
#include <observability/core_metrics.h>
#endif

namespace {
using PluginQuery = const ibmw_reporter_v1* (*)();

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "installed consumer failure: " << message << '\n';
    std::exit(1);
  }
}

void CheckPlugin(const ibmw_reporter_v1* expected, bool deepbind) {
  int flags = RTLD_NOW | RTLD_LOCAL;
#ifdef RTLD_DEEPBIND
  if (deepbind) flags |= RTLD_DEEPBIND;
#else
  (void)deepbind;
#endif
  void* plugin = dlopen(REPORTER_PLUGIN_PATH, flags);
  Require(plugin != nullptr, dlerror());
  auto query = reinterpret_cast<PluginQuery>(
      dlsym(plugin, "installed_consumer_plugin_query"));
  Require(query != nullptr, "plugin bridge symbol missing");
  Require(query() == expected, "plugin did not resolve host reporter owner");
  dlclose(plugin);
}
}  // namespace

int main() {
  unsetenv("IBMW_CORE_METRICS");
#if IBMW_EXPECT_REPORTER
  auto& controller = ibmw::runtime::core::observability::Controller::Instance();
  controller.ResetForTesting();
  controller.Initialize();
  Require(ibmw_reporter_query_v1() == nullptr, "runtime-off must return null");
  controller.ResetForTesting();
  Require(setenv("IBMW_CORE_METRICS", "counters", 1) == 0, "setenv failed");
  controller.Initialize();
  const auto* api = ibmw_reporter_query_v1();
  Require(api != nullptr, "counters reporter missing");
  Require(api->abi_version == IBMW_REPORTER_ABI_VERSION_V1, "bad version");
  Require(api->struct_size >= sizeof(ibmw_reporter_v1), "bad size");
  Require(api->register_slot && api->add && api->add_signed, "bad callbacks");
  ibmw_reporter_slot_descriptor_v1 descriptor{};
  descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  descriptor.struct_size = sizeof(descriptor);
  descriptor.kind = {"fixture", 7};
  descriptor.metric = {"counter", 7};
  auto* slot = api->register_slot(api->owner, &descriptor);
  Require(slot != nullptr, "register failed");
  api->add(api->owner, slot, 2);
  api->add_signed(api->owner, slot, -1);
  CheckPlugin(api, false);
  CheckPlugin(api, true);
  controller.ResetForTesting();
#else
  const auto* api = ibmw_reporter_query_v1();
  Require(api == nullptr, "compile-out reporter must return null");
  CheckPlugin(nullptr, false);
  CheckPlugin(nullptr, true);
#endif
  std::cout << "PASS installed reporter consumer\n";
}
