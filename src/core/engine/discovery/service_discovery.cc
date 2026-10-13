// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/discovery/service_discovery.h"

#include <exception>
#include <cstdlib>
#include <dlfcn.h>
#include <future>
#include <iterator>
#include <stdexcept>

#include "engine/runtime_graph/runtime_registry.h"

namespace ibmw::discovery {

namespace {
ServiceDiscoverySnapshot QuerySource(const ServiceDiscoveryOptions& options,
                                    std::chrono::steady_clock::time_point deadline) {
  if (options.backend == ServiceDiscoveryBackend::kRegistry)
    return DiscoverRegistryServices(options.registry_path);
  ServiceDiscoverySnapshot result;
  ServiceDiscoverySourceStatus status{.source = DescribeSource(options),
                                      .status = DiscoveryStatus::kError};
  void* library = nullptr;
  ServiceDiscoveryPluginV1 api;
  ServiceDiscoveryProvider* provider = nullptr;
  try {
    std::filesystem::path directory;
    if (const char* env = std::getenv("IBMW_DISCOVERY_PLUGIN_DIR"); env && *env) {
      directory = env;
    } else {
      static const char anchor = 0;
      Dl_info info{};
      if (!::dladdr(&anchor, &info) || !info.dli_fname)
        throw std::runtime_error("Cannot locate service discovery plugins.");
      directory = std::filesystem::absolute(info.dli_fname).parent_path();
    }
    const auto path = directory / ("libibmw_service_discovery_" +
                                   std::string(BackendName(options.backend)) + ".so");
    if (!std::filesystem::exists(path)) {
      status.status = DiscoveryStatus::kUnavailable;
      throw std::runtime_error("Service discovery backend is not installed: " + path.string());
    }
    library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) throw std::runtime_error(::dlerror());
    const auto entry = reinterpret_cast<ServiceDiscoveryPluginV1 (*)()>(
        ::dlsym(library, "IbmwServiceDiscoveryEntryV1"));
    if (!entry) throw std::runtime_error("Missing IbmwServiceDiscoveryEntryV1: " + path.string());
    api = entry();
    if (api.version != 1 || api.struct_size != sizeof(api) || api.backend != options.backend ||
        !api.create || !api.destroy)
      throw std::runtime_error("Incompatible service discovery plugin: " + path.string());
    provider = api.create();
    if (!provider) throw std::runtime_error("Service discovery plugin returned null.");
    result = provider->Query(options, deadline);
    if (result.sources.empty()) throw std::runtime_error("Service discovery returned no source status.");
  } catch (const std::exception& error) {
    status.message = error.what();
    result.sources.push_back(std::move(status));
  } catch (...) {
    status.message = "Unknown service discovery failure.";
    result.sources.push_back(std::move(status));
  }
  // Plugin exceptions must be destroyed while their library is still loaded.
  if (provider) api.destroy(provider);
  if (library) ::dlclose(library);
  return result;
}
}  // namespace

ServiceDiscoverySnapshot DiscoverServices(const std::vector<ServiceDiscoveryOptions>& sources,
                                         std::chrono::milliseconds timeout) {
  if (sources.empty() || timeout <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("Service discovery requires sources and a positive timeout.");
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::vector<std::future<ServiceDiscoverySnapshot>> pending;
  for (const auto& source : sources)
    pending.push_back(std::async(std::launch::async, QuerySource, source, deadline));
  ServiceDiscoverySnapshot result;
  for (auto& future : pending) {
    auto snapshot = future.get();
    result.services.insert(result.services.end(), std::make_move_iterator(snapshot.services.begin()),
                           std::make_move_iterator(snapshot.services.end()));
    result.sources.insert(result.sources.end(), std::make_move_iterator(snapshot.sources.begin()),
                          std::make_move_iterator(snapshot.sources.end()));
  }
  return result;
}

ServiceDiscoverySnapshot DiscoverRegistryServices(
    const std::filesystem::path& registry_path) {
  const ServiceDiscoverySource source{
      .backend = ServiceDiscoveryBackend::kRegistry,
      .instance_id = registry_path.lexically_normal().string()};
  ServiceDiscoverySnapshot result;
  ServiceDiscoverySourceStatus status{.source = source, .status = DiscoveryStatus::kReady};
  try {
    if (registry_path.empty()) throw std::invalid_argument("Registry path must not be empty.");
    if (std::filesystem::exists(registry_path) && !std::filesystem::is_directory(registry_path)) {
      throw std::invalid_argument("Registry root is not a directory: " + registry_path.string());
    }
    const runtime::core::runtime_graph::RuntimeRegistry registry(registry_path);
    for (const auto& runtime : registry.DiscoverLocalRuntimes()) {
      if (!runtime.services) {
        status.status = DiscoveryStatus::kError;
        if (!status.message.empty()) status.message += ' ';
        status.message += "Runtime '" + runtime.runtime_id +
            "' does not advertise service information; upgrade and restart it.";
        continue;
      }
      for (const auto& service : *runtime.services) {
        result.services.push_back({.source = source, .runtime_id = runtime.runtime_id,
                                   .advertisement = service});
      }
    }
  } catch (const std::exception& error) {
    status.status = DiscoveryStatus::kError;
    status.message = error.what();
  }
  result.sources.push_back(std::move(status));
  return result;
}

}  // namespace ibmw::discovery
