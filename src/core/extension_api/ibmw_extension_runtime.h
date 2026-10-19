// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "c/service/rpc_handle_base.h"
#include "cpp/executor/executor.h"
#include "cpp/logger/logger.h"
#include "cpp/transport/serialized_message.h"
#include "cpp/transport/transport_context.h"
#include "cpp/util/type_support.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::extension {

// V2 includes per-driver endpoint QoS. V1 binaries must be rejected before
// InitializeRuntime; all components use the same compiler/STL and headers.
inline constexpr uint32_t kRuntimeAbiVersionV2 = 2;

enum class RuntimePhase : uint8_t {
  kInitialize,
  kStart,
  kStopping,
};

enum class RuntimeSubsystem : uint8_t {
  kExecutor,
  kLogger,
  kModule,
  kAll,
  kRuntimeGraph,
};

enum class RuntimeHookTiming : uint8_t {
  kBefore,
  kAfter,
};

struct RuntimeHookPoint {
  RuntimePhase phase;
  RuntimeSubsystem subsystem;
  RuntimeHookTiming timing;
};

enum class CaptureMode : uint8_t {
  kRuntimeTap,
  kNativeSerialized,
  kCachedSerialized,
};

struct TransportCaptureCapabilityV1 {
  uint32_t abi_version = 0;
  CaptureMode mode = CaptureMode::kRuntimeTap;
  uint32_t priority = 100;
  bool supports_topic = true;
  std::string extension_name;
  std::string extension_path;
  std::string extension_options_yaml;
  std::string driver_options_yaml;
  std::string provider_instance_option;
  std::string provider_clear_option;
  std::string provider_strip_leading_slash_option;
};

struct TransportEndpointV2 {
  bool published = true;
  std::string topic;
  std::string msg_type;
  std::string package_path;
  std::string module_name;
  uint64_t index = 0;
  ibmw::util::TypeSupportRef type_support;
  std::vector<std::string> drivers;
  ibmw::transport::QosDescriptor qos;
  std::map<std::string, TransportCaptureCapabilityV1> capture_capabilities;
  std::map<std::string, ibmw::transport::QosDescriptor> driver_qos;
};

struct MessageView {
  std::string_view topic;
  std::string_view msg_type;
  std::string_view module_name;
  uint64_t endpoint_index = 0;
  ibmw::transport::ContextRef context;
  bool has_typed_message = false;
  std::unordered_set<std::string> available_serialization_types;
  std::function<std::shared_ptr<ibmw::util::BufferArrayView>(
      std::string_view serialization_type)>
      acquire_serialized;
};

struct SubscriptionOptions {
  std::string topic;
  std::string msg_type;
  std::string package_path;
  std::string module_name;
  ibmw::util::TypeSupportRef type_support;
  std::unordered_set<std::string> required_serialization_types;
};

struct PublisherOptions {
  std::string topic;
  std::string msg_type;
  std::string package_path;
  std::string module_name;
  ibmw::util::TypeSupportRef type_support;
};

struct PublisherHandle {
  const void* value = nullptr;

  explicit operator bool() const noexcept { return value != nullptr; }
};

class RuntimeV2 {
 public:
  using Hook = std::function<void()>;
  using MessageCallback =
      std::function<void(MessageView&, std::function<void()>&&)>;
  using PublishTap = std::function<void(MessageView&)>;

  RuntimeV2() = default;
  virtual ~RuntimeV2() = default;
  RuntimeV2(const RuntimeV2&) = delete;
  RuntimeV2& operator=(const RuntimeV2&) = delete;

  virtual uint32_t AbiVersion() const noexcept = 0;
  virtual YAML::Node GetExtensionOptions(std::string_view name) const = 0;
  virtual void UpdateExtensionOptions(
      std::string_view name, YAML::Node options) = 0;
  virtual YAML::Node GetRuntimeOptions(std::string_view key) = 0;
  virtual void RegisterHook(RuntimeHookPoint point, Hook hook) = 0;
  virtual ibmw::executor::ExecutorHandle GetExecutor(
      std::string_view name) const = 0;
  virtual ibmw::logger::LoggerRef GetLogger() const = 0;
  virtual const ibmw_service_handle_base_t* GetRpcHandle() const = 0;

  virtual std::vector<TransportEndpointV2> GetTransportEndpoints() const = 0;
  virtual bool Subscribe(
      SubscriptionOptions options, MessageCallback callback) = 0;
  virtual PublisherHandle RegisterPublisher(PublisherOptions options) = 0;
  virtual void Send(
      PublisherHandle publisher,
      ibmw::transport::ContextRef context,
      std::string serialization_type,
      std::shared_ptr<ibmw::util::BufferArrayView> payload) = 0;
  virtual uint64_t RegisterPublishTap(PublishTap tap) = 0;
  virtual bool UnregisterPublishTap(uint64_t handle) = 0;
};

}  // namespace ibmw::extension
