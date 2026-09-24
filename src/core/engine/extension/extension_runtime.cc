// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/extension/extension_runtime.h"

#include <algorithm>
#include <utility>

#include "engine/engine.h"
#include "engine/transport/serialized_transport_driver.h"
#include "engine/transport/transport_driver_tools.h"
#include "extension_api/ibmw_runtime_graph.h"

namespace ibmw::runtime::core::extension {
namespace {

class EngineRuntimeV2 final : public ibmw::extension::RuntimeV2,
                              public ibmw::extension::RuntimeGraphAccessV1 {
 public:
  explicit EngineRuntimeV2(Engine& engine) : engine_(engine) {}

  ibmw::extension::RuntimeGraphV1& GetRuntimeGraph() override {
    return engine_.GetRuntimeGraphSubsystem();
  }

  uint32_t AbiVersion() const noexcept override {
    return ibmw::extension::kRuntimeAbiVersionV2;
  }

  YAML::Node GetExtensionOptions(std::string_view name) const override {
    return engine_.GetExtensionBus().GetExtensionOptionsNode(name);
  }

  void UpdateExtensionOptions(
      std::string_view name, YAML::Node options) override {
    engine_.GetExtensionBus().UpdateExtensionOptionsNode(
        name, std::move(options));
  }

  YAML::Node GetRuntimeOptions(std::string_view key) override {
    return engine_.GetConfigSubsystem().GetIbmwOptionsNode(key);
  }

  void RegisterHook(
      ibmw::extension::RuntimeHookPoint point, Hook hook) override {
    engine_.RegisterHook(ToEngineHook(point), std::move(hook));
  }

  ibmw::executor::ExecutorHandle GetExecutor(
      std::string_view name) const override {
    return engine_.GetExecutorBus().GetExecutor(name);
  }

  ibmw::logger::LoggerRef GetLogger() const override {
    return ibmw::logger::LoggerRef(
        engine_.GetLoggingSubsystem().GetLoggerAccess().NativeHandle());
  }

  const ibmw_service_handle_base_t* GetRpcHandle() const override {
    return engine_.GetServiceDispatcher().GetRpcAccess().NativeHandle();
  }

  std::vector<ibmw::extension::TransportEndpointV2>
  GetTransportEndpoints() const override {
    namespace ext = ibmw::extension;
    namespace transport = ibmw::runtime::core::transport;

    const auto& bus = engine_.GetTransportBus();
    const auto* catalog = bus.GetTransportCatalog();
    if (catalog == nullptr) {
      return {};
    }

    std::unordered_map<std::string, std::pair<std::string, std::string>>
        extension_configs;
    for (const auto& item : engine_.GetExtensionBus().GetOptions().extensions_options) {
      extension_configs.emplace(
          item.name, std::make_pair(item.path, YAML::Dump(item.options)));
    }
    std::unordered_map<std::string, std::string> driver_configs;
    for (const auto& item : bus.GetOptions().drivers_options) {
      driver_configs.emplace(item.type, YAML::Dump(item.options));
    }
    std::unordered_map<std::string, transport::TransportDriver*> drivers;
    for (auto* driver : bus.GetUsedTransportDriver()) {
      drivers.emplace(std::string(driver->Name()), driver);
    }

    const auto publisher_drivers =
        bus.GetTransportDriverDispatcher().GetPubTopicDriverInfo();
    const auto subscriber_drivers =
        bus.GetTransportDriverDispatcher().GetSubTopicDriverInfo();
    std::vector<ext::TransportEndpointV2> result;

    const auto append = [&](const auto& topic_map, const auto& driver_map,
                            bool published) {
      for (const auto& [topic_name, wrappers] : topic_map) {
        std::vector<std::string> endpoint_drivers;
        if (const auto found = driver_map.find(topic_name);
            found != driver_map.end()) {
          endpoint_drivers.reserve(found->second.size());
          for (const auto driver : found->second) {
            endpoint_drivers.emplace_back(driver);
          }
        }
        for (const auto* wrapper : wrappers) {
          ext::TransportEndpointV2 endpoint{
              .published = published,
              .topic = std::string(topic_name),
              .msg_type = wrapper->info.msg_type,
              .package_path = wrapper->info.pkg_path,
              .module_name = wrapper->info.module_name,
              .index = wrapper->info.index,
              .type_support = wrapper->info.msg_type_support_ref,
              .drivers = endpoint_drivers};

          result.emplace_back(std::move(endpoint));
        }
      }
    };

    append(catalog->GetPubTopicIndexMap(), publisher_drivers, true);
    append(catalog->GetSubTopicIndexMap(), subscriber_drivers, false);

    for (const auto& pod : bus.GetTransportDriverDispatcher().GetLoanedPodTopics()) {
      const auto& info = pod.info;
      // Typed and POD registrations can describe the same logical publisher.
      if (catalog->GetSendTypeWrapperPtr(
              info.msg_type, info.topic_name, info.pkg_path, info.module_name)) {
        continue;
      }
      result.emplace_back(ext::TransportEndpointV2{
          .published = true,
          .topic = info.topic_name,
          .msg_type = info.msg_type,
          .package_path = info.pkg_path,
          .module_name = info.module_name,
          .index = info.index,
          .type_support = info.msg_type_support_ref,
          .drivers = {pod.driver}});
    }

    for (auto& endpoint : result) {
      const std::string serialization_type(
          endpoint.type_support.DefaultSerializationType());
      const ibmw::transport::TopicDescriptor topic{
          .topic = endpoint.topic,
          .msg_type = endpoint.msg_type,
          .serialization_type = serialization_type};
      for (const auto& driver_name : endpoint.drivers) {
        // Keep an entry even when a driver cannot report QoS.
        auto& driver_qos = endpoint.driver_qos[driver_name];
        const auto driver = drivers.find(driver_name);
        if (driver == drivers.end()) {
          continue;
        }
        const auto* capability =
            bus.GetSerializedTransportCapability(*driver->second);
        const bool valid_capability =
            capability != nullptr && capability->HasValidContract();
        const auto qos_query = valid_capability
                                   ? (endpoint.published
                                          ? capability->operations.publisher_qos
                                          : capability->operations.subscriber_qos)
                                   : nullptr;
        if (qos_query != nullptr) {
          driver_qos = qos_query(capability->operations.context,
                                endpoint.topic, endpoint.msg_type);
        }
        // A single transport's QoS is also consumed by bag storage and playback.
        if (endpoint.drivers.size() == 1) endpoint.qos = driver_qos;

        ext::TransportCaptureCapabilityV1 advertised;
        if (valid_capability) {
          advertised.abi_version = capability->abi_version;
          advertised.priority = capability->capture_priority;
          advertised.supports_topic =
              !serialization_type.empty() &&
              (capability->capture_mode !=
                   transport::SerializedCaptureMode::kNativeSerialized ||
               capability->operations.supports_topic(
                   capability->operations.context, topic));
          if (capability->capture_mode ==
              transport::SerializedCaptureMode::kNativeSerialized) {
            advertised.mode = ext::CaptureMode::kNativeSerialized;
          } else if (capability->capture_mode ==
                     transport::SerializedCaptureMode::kCachedSerialized) {
            advertised.mode = ext::CaptureMode::kCachedSerialized;
          }
          advertised.extension_name = capability->provider_name;
          advertised.provider_instance_option = capability->provider_instance_option;
          advertised.provider_clear_option = capability->provider_clear_option;
          advertised.provider_strip_leading_slash_option =
              capability->provider_strip_leading_slash_option;
          const auto extension = extension_configs.find(advertised.extension_name);
          const auto config = driver_configs.find(driver_name);
          if (extension != extension_configs.end() && config != driver_configs.end()) {
            advertised.extension_path = extension->second.first;
            advertised.extension_options_yaml = extension->second.second;
            advertised.driver_options_yaml = config->second;
          }
        }
        endpoint.capture_capabilities.emplace(driver_name, std::move(advertised));
      }
    }
    return result;
  }

  bool Subscribe(
      ibmw::extension::SubscriptionOptions options,
      MessageCallback callback) override {
    namespace transport = ibmw::runtime::core::transport;
    transport::MessageHandlerWrapper wrapper;
    wrapper.info = transport::TransportInfo{
        .msg_type = std::move(options.msg_type),
        .topic_name = std::move(options.topic),
        .pkg_path = std::move(options.package_path),
        .module_name = std::move(options.module_name),
        .msg_type_support_ref = options.type_support};
    wrapper.require_cache_serialization_types =
        std::move(options.required_serialization_types);
    wrapper.callback =
        [callback = std::move(callback)](
            transport::MessageFrame& frame,
            std::function<void()>&& release,
            const transport::internal::CallbackInvocationEnvelopeV1*) mutable {
          ibmw::extension::MessageView view{
              .topic = frame.info.topic_name,
              .msg_type = frame.info.msg_type,
              .module_name = frame.info.module_name,
              .endpoint_index = frame.info.index,
              .context = frame.ctx_ref,
              .has_typed_message = frame.msg_ptr != nullptr,
              .available_serialization_types = [&] {
                std::unordered_set<std::string> formats;
                for (const auto& [format, payload] : frame.serialization_cache) {
                  static_cast<void>(payload);
                  formats.emplace(format);
                }
                return formats;
              }(),
              .acquire_serialized = [&frame](std::string_view format) {
                return transport::TrySerializeMsgWithCache(frame, format);
              }};
          callback(view, std::move(release));
        };
    return engine_.GetTransportBus().OnMessage(std::move(wrapper));
  }

  ibmw::extension::PublisherHandle RegisterPublisher(
      ibmw::extension::PublisherOptions options) override {
    namespace transport = ibmw::runtime::core::transport;
    const auto* catalog = engine_.GetTransportBus().GetTransportCatalog();
    if (const auto* existing = catalog->GetSendTypeWrapperPtr(
            options.msg_type, options.topic, options.package_path,
            options.module_name)) {
      return {.value = existing};
    }
    transport::SendTypeWrapper wrapper;
    wrapper.info = transport::TransportInfo{
        .msg_type = std::move(options.msg_type),
        .topic_name = std::move(options.topic),
        .pkg_path = std::move(options.package_path),
        .module_name = std::move(options.module_name),
        .msg_type_support_ref = options.type_support};
    const auto info = wrapper.info;
    if (!engine_.GetTransportBus().RegisterSendType(std::move(wrapper))) {
      return {};
    }
    const auto* registered = engine_.GetTransportBus().GetTransportCatalog()
                                 ->GetSendTypeWrapperPtr(
                                     info.msg_type, info.topic_name,
                                     info.pkg_path, info.module_name);
    return {.value = registered};
  }

  void Send(
      ibmw::extension::PublisherHandle publisher,
      ibmw::transport::ContextRef context,
      std::string serialization_type,
      std::shared_ptr<ibmw::util::BufferArrayView> payload) override {
    namespace transport = ibmw::runtime::core::transport;
    const auto* wrapper =
        static_cast<const transport::SendTypeWrapper*>(publisher.value);
    MW_REQUIRE(wrapper != nullptr, "Invalid extension publisher handle.");
    transport::MessageFrame frame{
        .info = wrapper->info,
        .msg_ptr = nullptr,
        .ctx_ref = context};
    frame.serialization_cache.emplace(
        std::move(serialization_type), std::move(payload));
    engine_.GetTransportBus().Send(std::move(frame));
  }

  uint64_t RegisterPublishTap(PublishTap tap) override {
    namespace transport = ibmw::runtime::core::transport;
    return engine_.GetTransportBus().RegisterPublishTap(
        [tap = std::move(tap)](transport::MessageFrame& frame) mutable {
          ibmw::extension::MessageView view{
              .topic = frame.info.topic_name,
              .msg_type = frame.info.msg_type,
              .module_name = frame.info.module_name,
              .endpoint_index = frame.info.index,
              .context = frame.ctx_ref,
              .has_typed_message = frame.msg_ptr != nullptr,
              .available_serialization_types = [&] {
                std::unordered_set<std::string> formats;
                for (const auto& [format, payload] : frame.serialization_cache) {
                  static_cast<void>(payload);
                  formats.emplace(format);
                }
                return formats;
              }(),
              .acquire_serialized = [&frame](std::string_view format) {
                return transport::TrySerializeMsgWithCache(frame, format);
              }};
          tap(view);
        });
  }

  bool UnregisterPublishTap(uint64_t handle) override {
    return engine_.GetTransportBus().UnregisterPublishTap(handle);
  }

 private:
  static HookPoint ToEngineHook(ibmw::extension::RuntimeHookPoint point) {
    const auto phase = [&] {
      switch (point.phase) {
        case ibmw::extension::RuntimePhase::kInitialize:
          return Phase::kInitialize;
        case ibmw::extension::RuntimePhase::kStart:
          return Phase::kStart;
        case ibmw::extension::RuntimePhase::kStopping:
          return Phase::kStopping;
      }
      return Phase::kStopped;
    }();
    const auto subsystem = [&] {
      switch (point.subsystem) {
        case ibmw::extension::RuntimeSubsystem::kExecutor:
          return Subsystem::kExecutor;
        case ibmw::extension::RuntimeSubsystem::kLogger:
          return Subsystem::kLogger;
        case ibmw::extension::RuntimeSubsystem::kModule:
          return Subsystem::kModule;
        case ibmw::extension::RuntimeSubsystem::kAll:
          return Subsystem::kCount;
        case ibmw::extension::RuntimeSubsystem::kRuntimeGraph:
          return Subsystem::kRuntimeGraph;
      }
      return Subsystem::kCount;
    }();
    const auto timing =
        point.timing == ibmw::extension::RuntimeHookTiming::kBefore
            ? HookTiming::kBefore
            : HookTiming::kAfter;
    return {.phase = phase, .subsystem = subsystem, .timing = timing};
  }

  Engine& engine_;
};

}  // namespace

std::unique_ptr<ibmw::extension::RuntimeV2> MakeExtensionRuntimeV2(
    Engine& engine) {
  return std::make_unique<EngineRuntimeV2>(engine);
}

}  // namespace ibmw::runtime::core::extension
