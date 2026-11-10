// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "shm_ext/shm_extension.h"

#include <unistd.h>

#include <stdexcept>

#include "shm_ext/global.h"
#include "shm_ext/shm_transport_driver.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::shm_extension::ShmExtension::Options> {
  using Options = ibmw::extensions::shm_extension::ShmExtension::Options;

  static Node encode(const Options& rhs) {
    Node node;
    node["shm_init_size"] = rhs.shm_init_size;
    node["runtime_id"] = rhs.runtime_id;
    node["domain"] = rhs.domain;
    node["root_dir"] = rhs.root_dir;
    node["max_bytes_per_publisher"] = rhs.max_bytes_per_publisher;
    node["prefault"] = rhs.prefault;
    node["driver_name"] = rhs.driver_name;
    if (rhs.group) node["group"] = *rhs.group;
    node["require_read_only"] = rhs.require_read_only;
    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;
    if (node["shm_init_size"]) rhs.shm_init_size = node["shm_init_size"].as<uint64_t>();
    if (node["runtime_id"].IsDefined()) rhs.runtime_id = node["runtime_id"].as<std::string>();
    if (node["domain"]) rhs.domain = node["domain"].as<std::string>();
    if (node["root_dir"]) rhs.root_dir = node["root_dir"].as<std::string>();
    if (node["max_bytes_per_publisher"])
      rhs.max_bytes_per_publisher = node["max_bytes_per_publisher"].as<uint64_t>();
    if (node["prefault"]) rhs.prefault = node["prefault"].as<bool>();
    if (node["driver_name"]) rhs.driver_name = node["driver_name"].as<std::string>();
    if (const auto group = node["group"]) {
      const std::string text = group.IsScalar() ? group.Scalar() : std::string("<not a scalar>");
      rhs.group = ibmw::extensions::shm_extension::ParseShmGroupId(text);
      if (!rhs.group) {
        throw std::runtime_error("shm_extension: group must be a numeric gid below 4294967295, got '" + text + "'");
      }
    }
    if (node["require_read_only"]) rhs.require_read_only = node["require_read_only"].as<bool>();
    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::shm_extension {

ShmExtension::Options ParseShmExtensionOptions(const YAML::Node& node) { return node.as<ShmExtension::Options>(); }

ShmBusOptions MakeShmBusOptions(const ShmExtension::Options& options) {
  ShmBusOptions bus_options;
  bus_options.domain = options.domain;
  bus_options.root_dir = options.root_dir;
  bus_options.group = options.group;
  bus_options.runtime_name = options.runtime_id.empty() ? "shm" + std::to_string(::getpid()) : options.runtime_id;
  bus_options.shm_init_size = options.shm_init_size;
  bus_options.max_bytes_per_publisher = options.max_bytes_per_publisher;
  bus_options.prefault = options.prefault;
  bus_options.require_read_only = options.require_read_only;
  return bus_options;
}

std::string FindDriverNameConflict(std::string_view driver_name, const std::vector<std::string>& extension_names) {
  if (driver_name == "local") return "the built-in local transport";
  // net_extension registers tcp, udp and http; the others register "<x>" from "<x>_extension".
  const bool net = driver_name == "tcp" || driver_name == "udp" || driver_name == "http";
  const std::string owner = net ? "net_extension" : std::string(driver_name) + "_extension";
  if (!net && owner == "net_extension") return {};  // net_extension registers no "net" driver
  for (const auto& name : extension_names) {
    if (name == owner && name != "shm_extension") return "extension '" + name + "'";
  }
  return {};
}

ShmExtension::ShmExtension() { SetExtensionLogger(&g_logger); }

bool ShmExtension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());
  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = ParseShmExtensionOptions(extension_options_node);
  }
  MW_REQUIRE(!options_.driver_name.empty(), "shm_extension: driver_name must not be empty.");
  std::vector<std::string> extension_names;
  for (const auto& ext : core_ptr_->GetExtensionBus().GetOptions().extensions_options) {
    extension_names.push_back(ext.name);
  }
  const std::string conflict = FindDriverNameConflict(options_.driver_name, extension_names);
  MW_REQUIRE(conflict.empty(),
             "shm_extension: driver_name '{}' is also registered by {}; the engine would use only one of them. "
             "Load only one of the two extensions, or give shm_extension another driver_name.",
             options_.driver_name, conflict);

  shm_bus_->Initialize(MakeShmBusOptions(options_));

  core_ptr_->RegisterHook(
      runtime::core::Before(runtime::core::Phase::kInitialize, runtime::core::Subsystem::kTransport),
      [this] { RegisterShmTransportDriver(); });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);
  return true;
}

ShmExtension::~ShmExtension() {
  // Drivers and loan handles refer to the bus; if something still uses it
  // (a blocked publish, a held loan) keep it alive for the rest of the process.
  if (!shm_bus_->TryPrepareShutdown(std::chrono::seconds(1))) {
    MW_ERROR("shm_extension: shm bus still in use at destruction; leaking it.");
    (void)shm_bus_.release();
  }
}

void ShmExtension::OnShutdown() {
  // The transport bus normally prepared and shut the driver down already;
  // this covers engines that stop before the transport subsystem ran.
  if (!shm_bus_->TryPrepareShutdown(std::chrono::seconds(1))) {
    MW_ERROR("shm_extension: shutdown could not prepare the shm bus; resources are kept.");
    return;
  }
  shm_bus_->Shutdown();
}

void ShmExtension::RegisterShmTransportDriver() {
  auto concrete_driver = std::make_unique<ShmTransportDriver>(*shm_bus_, options_.driver_name);
  auto* concrete_ptr = concrete_driver.get();
  std::unique_ptr<runtime::core::transport::TransportDriver> driver_ptr = std::move(concrete_driver);

  core_ptr_->GetTransportBus().RegisterTransportDriver(
      std::move(driver_ptr),
      runtime::core::transport::TransportDriverCapabilitiesV1{
          .shutdown_v1 = static_cast<runtime::core::transport::TransportShutdownCapabilityV1*>(concrete_ptr),
          .loaned_pod_registration_v1 =
              static_cast<runtime::core::transport::LoanedPodRegistrationCapabilityV1*>(concrete_ptr),
          .retained_loan_v1 = static_cast<runtime::core::transport::RetainedLoanCapabilityV1*>(concrete_ptr)},
      runtime::core::transport::SerializedTransportCapabilityV2{
          .capture_mode = runtime::core::transport::SerializedCaptureMode::kCachedSerialized,
          .publish_mode = runtime::core::transport::SerializedPublishMode::kBus,
          .serialization = runtime::core::transport::SerializedFormatSupport::kTypeSupport,
          .schema = runtime::core::transport::SerializedSchemaSupport::kTypeSupportPackage,
          .loan = runtime::core::transport::SerializedLoanSupport::kSharedPayload,
          .callback_thread = runtime::core::transport::SerializedCallbackThread::kTransportManaged,
          .capture_priority = 0,
          .provider_name = Name(),
          .provider_instance_option = "runtime_id"});
}

}  // namespace ibmw::extensions::shm_extension

IBMW_EXT_ENTRY(ibmw::extensions::shm_extension::ShmExtension)
