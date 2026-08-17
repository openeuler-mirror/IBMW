#include <extension_api/ibmw_reporter_v1.h>

#include <dlfcn.h>

extern "C" __attribute__((visibility("default"))) const ibmw_reporter_v1*
installed_consumer_plugin_query() {
  using Query = const ibmw_reporter_v1* (*)();
  void* host = dlopen(nullptr, RTLD_NOW | RTLD_LOCAL);
  if (!host) return nullptr;
  auto query = reinterpret_cast<Query>(dlsym(host, "ibmw_reporter_query_v1"));
  const auto* result = query ? query() : nullptr;
  dlclose(host);
  return result;
}
