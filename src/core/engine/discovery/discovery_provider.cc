// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/discovery/discovery_provider.h"

#include <cstdlib>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <iterator>
#include <stdexcept>

#include <sys/stat.h>
#include <unistd.h>

#include "engine/discovery/registry_discovery_provider.h"

namespace ibmw::discovery {

namespace {

// The override for tests and development setups. secure_getenv ignores it in
// setuid processes.
const char* PluginDirectoryOverride() {
  const char* value = ::secure_getenv("IBMW_DISCOVERY_PLUGIN_DIR");
  return value && *value ? value : nullptr;
}

// An overridden plugin must not let another user plant code, for example
// through an environment kept by sudo: its directory and the library belong to
// this user or root and are not world writable. Group write is accepted for
// user private groups (umask 002).
void RequireTrustedPlugin(const std::filesystem::path& path) {
  for (const auto& item : {path.parent_path(), path}) {
    struct stat status {};
    if (::stat(item.c_str(), &status) != 0 ||
        (status.st_uid != ::geteuid() && status.st_uid != 0) ||
        (status.st_mode & S_IWOTH) != 0) {
      throw std::runtime_error("Untrusted discovery plugin '" + path.string() + "': '" +
                               item.string() +
                               "' must belong to the current user or root and not be world writable.");
    }
  }
}

std::filesystem::path PluginDirectory() {
  if (const char* value = PluginDirectoryOverride()) {
    return value;
  }
  static const char anchor = 0;
  Dl_info info{};
  if (::dladdr(&anchor, &info) == 0 || info.dli_fname == nullptr) {
    throw std::runtime_error("Cannot locate the discovery plugin directory.");
  }
  return std::filesystem::absolute(info.dli_fname).parent_path();
}

// Never let a plugin-owned exception outlive the DSO. In particular, a caller
// can destroy this provider while unwinding a failed enclosing constructor.
template <typename Function>
auto CallPlugin(Function&& function) -> decltype(function()) {
  std::string message;
  try {
    return function();
  } catch (const std::exception& error) {
    message = error.what();
  } catch (...) {
    message = "Unknown discovery plugin failure.";
  }
  // The original exception has been destroyed before throwing a host error.
  if (message.empty()) message = "Discovery plugin failed without a diagnostic.";
  throw std::runtime_error(message);
}

class PluginProvider final : public DiscoveryProvider {
 public:
  PluginProvider(const std::filesystem::path& path, DiscoveryBackend backend) {
    library_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library_) {
      const char* error = ::dlerror();
      throw std::runtime_error("Cannot load discovery plugin '" + path.string() + "': " +
                               (error ? error : "unknown error"));
    }
    std::string initialization_error;
    bool initialization_failed = false;
    try {
      const auto entry = reinterpret_cast<DiscoveryPluginV1 (*)()>(
          ::dlsym(library_, "IbmwDiscoveryEntryV1"));
      if (!entry) throw std::runtime_error("Missing IbmwDiscoveryEntryV1: " + path.string());
      api_ = entry();
      if (api_.version != 1 || api_.struct_size != sizeof(DiscoveryPluginV1) ||
          api_.backend != backend || api_.create == nullptr || api_.destroy == nullptr) {
        throw std::runtime_error("Incompatible discovery plugin: " + path.string());
      }
      provider_ = api_.create();
      if (!provider_) throw std::runtime_error("Discovery plugin returned a null provider: " + path.string());
    } catch (const std::exception& error) {
      initialization_failed = true;
      initialization_error = error.what();
    } catch (...) {
      initialization_failed = true;
      initialization_error = "Unknown discovery plugin initialization failure.";
    }
    // Destroy plugin exceptions before unloading the code that owns their type.
    if (initialization_failed) {
      ::dlclose(library_);
      if (initialization_error.empty()) initialization_error = "Discovery plugin initialization failed without a diagnostic.";
      throw std::runtime_error(initialization_error);
    }
  }
  ~PluginProvider() override {
    Stop();
    if (provider_) api_.destroy(provider_);
    if (library_) ::dlclose(library_);
  }
  void Start(const DiscoveryOptions& options) override {
    CallPlugin([&] { provider_->Start(options); });
  }
  DiscoverySnapshot Query(std::chrono::steady_clock::time_point deadline) override {
    return CallPlugin([&] { return provider_->Query(deadline); });
  }
  void Stop() noexcept override { if (provider_) provider_->Stop(); }

 private:
  void* library_ = nullptr;
  DiscoveryPluginV1 api_{};
  DiscoveryProvider* provider_ = nullptr;
};

}  // namespace

std::unique_ptr<DiscoveryProvider> CreateDiscoveryProvider(DiscoveryBackend backend) {
  if (backend == DiscoveryBackend::kRegistry) {
    return std::make_unique<RegistryDiscoveryProvider>();
  }
  const char* library = nullptr;
  switch (backend) {
    case DiscoveryBackend::kDds: library = "libibmw_discovery_dds.so"; break;
    case DiscoveryBackend::kRos2: library = "libibmw_discovery_ros2.so"; break;
    case DiscoveryBackend::kZenoh: library = "libibmw_discovery_zenoh.so"; break;
    default: break;
  }
  if (library) {
    const auto path = PluginDirectory() / library;
    if (std::filesystem::exists(path)) {
      if (PluginDirectoryOverride()) RequireTrustedPlugin(path);
      return std::make_unique<PluginProvider>(path, backend);
    }
  }
  return nullptr;
}

DiscoverySnapshot Discover(
    const std::vector<DiscoveryOptions>& sources,
    std::chrono::milliseconds timeout) {
  if (sources.empty() || timeout < std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("Discovery requires sources and a nonnegative timeout.");
  }
  struct Observer {
    std::unique_ptr<DiscoveryProvider> provider;
    DiscoverySnapshot snapshot;
  };
  std::vector<Observer> observers(sources.size());
  for (size_t index = 0; index < sources.size(); ++index) {
    const auto& options = sources[index];
    auto& observer = observers[index];
    try {
      observer.provider = CreateDiscoveryProvider(options.backend);
      if (!observer.provider) {
        observer.snapshot.sources.push_back(DiscoverySourceStatus{
            .source = DescribeSource(options), .status = DiscoveryStatus::kUnavailable,
            .message = "Discovery backend is not available in this installation."});
        continue;
      }
      observer.provider->Start(options);
    } catch (const std::exception& error) {
      observer.snapshot.sources.push_back(DiscoverySourceStatus{
          .source = DescribeSource(options), .status = DiscoveryStatus::kError,
          .message = error.what()});
    } catch (...) {
      observer.snapshot.sources.push_back(DiscoverySourceStatus{
          .source = DescribeSource(options), .status = DiscoveryStatus::kError,
          .message = "Unknown discovery initialization failure."});
    }
  }
  // Start all observers before the shared collection window. DDS maintains
  // its cache on middleware threads while other sources are being queried.
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (size_t index = 0; index < sources.size(); ++index) {
    auto& observer = observers[index];
    if (!observer.snapshot.sources.empty()) continue;
    try {
      observer.snapshot = observer.provider->Query(deadline);
    } catch (const std::exception& error) {
      observer.snapshot.sources.push_back({.source = DescribeSource(sources[index]),
                                           .status = DiscoveryStatus::kError,
                                           .message = error.what()});
    } catch (...) {
      observer.snapshot.sources.push_back({.source = DescribeSource(sources[index]),
                                           .status = DiscoveryStatus::kError,
                                           .message = "Unknown discovery query failure."});
    }
  }
  DiscoverySnapshot result;
  for (auto& observer : observers) {
    if (observer.provider) observer.provider->Stop();
    auto& snapshot = observer.snapshot;
    result.endpoints.insert(result.endpoints.end(),
                            std::make_move_iterator(snapshot.endpoints.begin()),
                            std::make_move_iterator(snapshot.endpoints.end()));
    result.sources.insert(result.sources.end(),
                          std::make_move_iterator(snapshot.sources.begin()),
                          std::make_move_iterator(snapshot.sources.end()));
  }
  return result;
}

}  // namespace ibmw::discovery
