// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/topic_io/registry_subscription.h"

#include <fstream>
#include <set>
#include <unistd.h>

#include "base/exception.h"
#include "base/log_util.h"
#include "engine/engine.h"
#include "engine/runtime_graph/type_support_registry.h"
#include "engine/topic_io/cached_transport_subscription_source.h"
#include "engine/topic_io/transport_subscription_source.h"

namespace ibmw::runtime::core::topic_io {
namespace {
std::string TypedKey(const RegistrySubscription& subscription) {
  auto publisher = subscription.publisher;
  publisher.runtime_id.clear();
  publisher.publisher_id.clear();
  return RegistrySubscriptionKey({std::move(publisher), subscription.driver});
}

class TemporaryConfig {
 public:
  explicit TemporaryConfig(const YAML::Node& config) {
    path_ = (std::filesystem::temp_directory_path() / "ibmw-registry-subscription-XXXXXX").string();
    const int fd = ::mkstemp(path_.data());
    MW_ASSERT(fd >= 0, "Cannot create registry subscription configuration.");
    ::close(fd);
    try {
      std::ofstream file(path_);
      file << config;
      file.close();
      MW_ASSERT(file.good(), "Cannot write registry subscription configuration.");
    } catch (...) {
      std::filesystem::remove(path_);
      throw;
    }
  }
  ~TemporaryConfig() { std::error_code error; std::filesystem::remove(path_, error); }
  const std::string& Path() const { return path_; }
 private:
  std::string path_;
};
}  // namespace

class RegistrySubscriptionRuntime::Impl {
 public:
  struct Context {
    std::string connection;
    runtime_graph::TypeSupportRegistry types;
    std::unique_ptr<CachedTransportSubscriptionRegistry> cached;
    std::unique_ptr<Engine> engine;
    transport::TransportDriver* driver = nullptr;
    transport::SerializedTransportCapabilityV2 capability;
    std::set<std::string> typed_subscriptions;
    std::future<void> future;
    bool started = false;

    ~Context() {
      try {
        Stop();
      } catch (const std::exception& error) {
        MW_LOG(ibmw::common::util::LoggerWrapper{}, ibmw::common::util::kLogLevelError,
               "Failed to stop registry subscription runtime: {}.", error.what());
      } catch (...) {
        MW_LOG(ibmw::common::util::LoggerWrapper{}, ibmw::common::util::kLogLevelError,
               "Failed to stop registry subscription runtime with an unknown error.");
      }
    }
    void Stop() {
      if (!engine) return;
      engine->Shutdown();
      if (future.valid()) future.wait();
      engine.reset();
      cached.reset();
    }
  };

  bool Available(const RegistrySubscription& subscription) const {
    const auto found = contexts.find(subscription.driver);
    if (found == contexts.end()) return true;
    const auto& context = *found->second;
    if (context.connection != RegistrySubscriptionConnectionKey(subscription)) return false;
    if (context.capability.capture_mode == transport::SerializedCaptureMode::kNativeSerialized)
      return context.capability.operations.supports_topic(context.capability.operations.context, subscription.publisher.topic);
    return !context.started || context.typed_subscriptions.contains(TypedKey(subscription));
  }

  std::unique_ptr<SerializedMessageSource> Create(const RegistrySubscription& subscription) {
    MW_ASSERT(Available(subscription), "Registry subscription route '{}' is unavailable.", subscription.driver);
    auto found = contexts.find(subscription.driver);
    if (found == contexts.end()) {
      auto context = std::make_unique<Context>();
      context->connection = RegistrySubscriptionConnectionKey(subscription);
      TemporaryConfig file(RegistrySubscriptionConfig(subscription));
      context->engine = std::make_unique<Engine>();
      context->engine->Initialize({.cfg_file_path = file.Path()});
      auto& bus = context->engine->GetTransportBus();
      for (auto* driver : bus.GetUsedTransportDriver()) {
        if (driver->Name() != subscription.driver) continue;
        const auto* capability = bus.GetSerializedTransportCapability(*driver);
        MW_ASSERT(capability && capability->HasValidContract(), "Invalid registry capture driver '{}'.", subscription.driver);
        context->driver = driver;
        context->capability = *capability;
      }
      MW_ASSERT(context->driver, "Registry capture driver '{}' is unavailable.", subscription.driver);
      context->cached = std::make_unique<CachedTransportSubscriptionRegistry>(context->engine->GetLogger());
      found = contexts.emplace(subscription.driver, std::move(context)).first;
    }
    auto& context = *found->second;
    const auto& publisher = subscription.publisher;
    const auto& advertised = publisher.capture_capabilities.at(subscription.driver);
    if (context.capability.capture_mode == transport::SerializedCaptureMode::kNativeSerialized) {
      MW_ASSERT(advertised.mode == "native_serialized" &&
                    context.capability.operations.supports_topic(context.capability.operations.context, publisher.topic),
                "Registry capture driver '{}' does not support the advertised native subscription.", subscription.driver);
      return std::make_unique<TransportSubscriptionSource>(subscription.driver, context.capability.operations,
          RegistrySubscriberOptions(subscription), context.engine->GetLogger());
    }
    MW_ASSERT(advertised.mode == "cached_serialized" &&
                  context.capability.capture_mode == transport::SerializedCaptureMode::kCachedSerialized,
              "Registry capture driver '{}' does not support the advertised typed subscription.", subscription.driver);
    context.types.Load(publisher.type_support_package);
    const auto type = context.types.Find(publisher.topic.msg_type, publisher.type_support_package);
    MW_ASSERT(type, "TypeSupport '{}' was not loaded.", publisher.topic.msg_type);
    context.typed_subscriptions.insert(TypedKey(subscription));
    return std::make_unique<CachedTransportSubscriptionSource>(subscription.driver, "ibmw-registry-observer",
        *context.driver, publisher, type, *context.cached, publisher.topic.qos);
  }

  std::map<std::string, std::unique_ptr<Context>> contexts;
};

RegistrySubscriptionRuntime::RegistrySubscriptionRuntime() : impl_(std::make_unique<Impl>()) {}
RegistrySubscriptionRuntime::~RegistrySubscriptionRuntime() = default;
bool RegistrySubscriptionRuntime::Available(const RegistrySubscription& subscription) const { return impl_->Available(subscription); }
std::unique_ptr<SerializedMessageSource> RegistrySubscriptionRuntime::Create(const RegistrySubscription& subscription) {
  return impl_->Create(subscription);
}
void RegistrySubscriptionRuntime::StartPending() {
  for (auto& [driver, context] : impl_->contexts) {
    if (context->started) continue;
    context->future = context->engine->AsyncStart();
    context->started = true;
  }
}
void RegistrySubscriptionRuntime::Stop() {
  std::exception_ptr error;
  for (auto& [driver, context] : impl_->contexts) {
    try { context->Stop(); } catch (...) { if (!error) error = std::current_exception(); }
  }
  impl_->contexts.clear();
  if (error) std::rethrow_exception(error);
}

}  // namespace ibmw::runtime::core::topic_io
