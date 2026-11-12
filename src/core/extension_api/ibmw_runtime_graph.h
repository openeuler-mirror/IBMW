// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpp/transport/topic_descriptor.h"
#include "cpp/util/type_support.h"
#include "extension_api/ibmw_runtime_directory.h"

namespace ibmw::extension {

inline constexpr uint32_t kRuntimeProtocolVersion = 2;

// Configuration supplies a root. Every protocol user adds its own namespace,
// including for explicit paths, so an older non-recursive reader cannot scan it.
inline std::filesystem::path RuntimeRegistryDirectory(
    const std::filesystem::path& root) {
  return root / ("v" + std::to_string(kRuntimeProtocolVersion));
}

struct TransportCaptureCapability {
  uint32_t abi_version = 0;
  std::string mode;
  uint32_t priority = 100;
  std::string extension_name;
  std::string extension_path;
  std::string extension_options_yaml;
  std::string driver_options_yaml;
  std::string provider_instance_option;
  std::string provider_clear_option;
  std::string provider_strip_leading_slash_option;
};

struct TopicAdvertisement {
  std::string runtime_id;
  std::string control_socket;
  std::string publisher_id;
  ibmw::transport::TopicDescriptor topic;
  std::string type_support_package;
  std::map<std::string, TransportCaptureCapability> capture_capabilities;
  bool published = true;
  bool hidden = false;
  std::string module_name;
  std::map<std::string, ibmw::transport::QosDescriptor> driver_qos;
};

// One registered RPC method, independent of the driver used to call it.
// Generic IBMW RPC has request/response types, not necessarily a .srv type.
struct ServiceAdvertisement {
  std::string endpoint_id;
  std::string service_name;
  std::string request_type;
  std::string response_type;
  std::string module_name;
  std::string package_path;
  std::vector<std::string> available_drivers;
  bool server = true;
  bool hidden = false;

  bool operator==(const ServiceAdvertisement&) const = default;
};

struct RuntimeRegistration {
  uint32_t protocol_version = kRuntimeProtocolVersion;
  std::string runtime_id;
  int64_t pid = 0;
  uint64_t process_start_time_ticks = 0;
  std::string control_socket;
  std::vector<TopicAdvertisement> topics;
  // Absent in older registrations; an empty list means no registered modules.
  // Names come from ModuleBus, including app-mode runtime contexts.
  std::optional<std::vector<std::string>> modules;
  // Absent in older registrations; an empty list means no registered RPCs.
  std::optional<std::vector<ServiceAdvertisement>> services;
};

struct TapSessionDescriptor {
  std::string session_id;
  std::string stream_socket;
};

struct RuntimeGraphOptions {
  std::optional<bool> enabled;
  std::optional<std::string> runtime_id;
  std::optional<std::filesystem::path> registry_path;
  std::vector<std::string> type_support_pkgs;
};

class RuntimeGraphV1 {
 public:
  using StartTapHandler =
      std::function<TapSessionDescriptor(const TopicAdvertisement&)>;
  using StopTapHandler = std::function<bool(std::string_view session_id)>;
  using StartPlaybackHandler =
      std::function<TapSessionDescriptor(const TopicAdvertisement&)>;
  using StopPlaybackHandler = std::function<bool(std::string_view session_id)>;

  virtual ~RuntimeGraphV1() = default;
  // Explicit runtime_graph settings take precedence; TypeSupport packages merge.
  virtual void ApplyFallbackOptions(const RuntimeGraphOptions& options) = 0;
  virtual ibmw::util::TypeSupportRef FindTypeSupport(
      std::string_view type, std::string_view package = {}) const = 0;
  virtual std::string TypeSupportPackage(
      std::string_view type, std::string_view package = {}) const = 0;
  virtual std::optional<ibmw::util::SchemaDescriptor> FindSchema(
      std::string_view type, std::string_view serialization) const = 0;
  virtual void HideModulePublishers(std::string module) = 0;
  virtual void SetStartTapHandler(StartTapHandler handler) = 0;
  virtual void SetStopTapHandler(StopTapHandler handler) = 0;
  virtual void SetStartPlaybackHandler(StartPlaybackHandler handler) = 0;
  virtual void SetStopPlaybackHandler(StopPlaybackHandler handler) = 0;
  virtual const RuntimeRegistration& Registration() const = 0;
  virtual bool Enabled() const noexcept = 0;
  virtual const std::filesystem::path& RegistryPath() const noexcept = 0;
};

// Optional capability queried independently of the existing RuntimeV2 vtable.
class RuntimeGraphAccessV1 {
 public:
  virtual ~RuntimeGraphAccessV1() = default;
  virtual RuntimeGraphV1& GetRuntimeGraph() = 0;
};

}  // namespace ibmw::extension
