// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "dds_ext/dds_extension.h"
#include "dds_ext/dds_transport_driver.h"
#include "dds_ext/dds_rpc_driver.h"
#include "dds_ext/global.h"
#include "yaml-cpp/yaml.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::dds_extension::DdsExtension::Options> {
  using Options = ibmw::extensions::dds_extension::DdsExtension::Options;
  using TransportPolicy = ibmw::extensions::dds_extension::DdsExtension::TransportPolicy;

  static const char* PolicyToStr(TransportPolicy p) {
    switch (p) {
      case TransportPolicy::kShmOnly: return "shm_only";
      case TransportPolicy::kTcpOnly: return "tcp_only";
      case TransportPolicy::kDefault: default: return "default";
    }
  }

  static bool StrToPolicy(const std::string& s, TransportPolicy& out) {
    if (s == "default")  { out = TransportPolicy::kDefault;  return true; }
    if (s == "shm_only") { out = TransportPolicy::kShmOnly;  return true; }
    if (s == "tcp_only") { out = TransportPolicy::kTcpOnly;  return true; }
    return false;
  }

  static Node encode(const Options& rhs) {
    Node node;
    node["domain_id"] = rhs.domain_id;
    node["participant_name"] = rhs.participant_name;
    if (!rhs.xml_profile_file.empty())
      node["xml_profile_file"] = rhs.xml_profile_file;
    if (!rhs.xml_profile_name.empty())
      node["xml_profile_name"] = rhs.xml_profile_name;
    node["transport_policy"] = std::string(PolicyToStr(rhs.transport_policy));
    node["tcp_listen_port"] = rhs.tcp_listen_port;
    node["tcp_peer_port"] = rhs.tcp_peer_port;
    node["tcp_addr"] = rhs.tcp_addr;
    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["domain_id"])
      rhs.domain_id = node["domain_id"].as<uint32_t>();

    if (node["participant_name"])
      rhs.participant_name = node["participant_name"].as<std::string>();

    if (node["xml_profile_file"])
      rhs.xml_profile_file = node["xml_profile_file"].as<std::string>();

    if (node["xml_profile_name"])
      rhs.xml_profile_name = node["xml_profile_name"].as<std::string>();

    if (node["transport_policy"]) {
      auto s = node["transport_policy"].as<std::string>();
      if (!StrToPolicy(s, rhs.transport_policy)) {
        return false;
      }
    }

    if (node["tcp_listen_port"])
      rhs.tcp_listen_port = node["tcp_listen_port"].as<uint16_t>();

    if (node["tcp_peer_port"])
      rhs.tcp_peer_port = node["tcp_peer_port"].as<uint16_t>();

    if (node["tcp_addr"])
      rhs.tcp_addr = node["tcp_addr"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::dds_extension {

DdsExtension::DdsExtension() {
  SetExtensionLogger(&g_logger);
}

bool DdsExtension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }

  // Translate extension transport policy into bus-level config.
  DdsBusTransportConfig bus_transport_config;
  switch (options_.transport_policy) {
    case DdsExtension::TransportPolicy::kDefault:
      bus_transport_config.policy = DdsBusTransportPolicy::kDefault;
      break;
    case DdsExtension::TransportPolicy::kShmOnly:
      bus_transport_config.policy = DdsBusTransportPolicy::kShmOnly;
      break;
    case DdsExtension::TransportPolicy::kTcpOnly:
      bus_transport_config.policy = DdsBusTransportPolicy::kTcpOnly;
      break;
  }
  bus_transport_config.tcp_listen_port = options_.tcp_listen_port;
  bus_transport_config.tcp_peer_port = options_.tcp_peer_port;
  bus_transport_config.tcp_addr = options_.tcp_addr;
  DdsBusXmlProfileConfig xml_profile_config;
  xml_profile_config.file = options_.xml_profile_file;
  xml_profile_config.participant_profile = options_.xml_profile_name;

  // Create DDS Bus (but don't initialize yet - wait for logger)
  dds_bus_ = std::make_unique<DdsBus>(options_.domain_id,
                                       options_.participant_name,
                                       bus_transport_config,
                                       xml_profile_config);

  core_ptr_->RegisterHook(runtime::core::After(runtime::core::Phase::kInitialize,
                                                runtime::core::Subsystem::kLogger),
                          [this] { InitDdsBus(); });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kTransport),
                          [this] { RegisterDdsTransportDriver(); });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kService),
                          [this] { RegisterDdsRpcDriver(); });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void DdsExtension::OnShutdown() {
  stop_flag_.store(true);

  if (dds_bus_) {
    dds_bus_->Shutdown();
  }

  SetDdsTransportDriver(nullptr);

  MW_INFO("DDS extension shutdown.");
}

void DdsExtension::InitDdsBus() {
  // Now that logger is set up, initialize DDS Bus
  dds_bus_->Initialize();

  MW_INFO("DDS extension initialized, domain_id: {}, participant_name: {}.",
             options_.domain_id, options_.participant_name);
}

void DdsExtension::RegisterDdsTransportDriver() {
  std::unique_ptr<runtime::core::transport::TransportDriver> dds_transport_driver_ptr =
      std::make_unique<DdsTransportDriver>(*dds_bus_);

  // Register executor function for DDS transport driver
  auto get_executor_func = [this](std::string_view executor_name) -> ibmw::executor::ExecutorHandle {
    return core_ptr_->GetExecutorBus().GetExecutor(executor_name);
  };

  auto* driver_ptr = static_cast<DdsTransportDriver*>(dds_transport_driver_ptr.get());
  driver_ptr->RegisterGetExecutorFunc(get_executor_func);

  // Set global driver pointer for LoanedMessage API access
  SetDdsTransportDriver(driver_ptr);

  core_ptr_->GetTransportBus().RegisterTransportDriver(
      std::move(dds_transport_driver_ptr));
}

void DdsExtension::RegisterDdsRpcDriver() {
  std::unique_ptr<runtime::core::service::ServiceDriver> dds_rpc_driver_ptr =
      std::make_unique<DdsRpcDriver>(*dds_bus_);

  auto* driver_ptr = static_cast<DdsRpcDriver*>(dds_rpc_driver_ptr.get());
  driver_ptr->RegisterGetExecutorFunc(
      [this](std::string_view executor_name) -> ibmw::executor::ExecutorHandle {
        return core_ptr_->GetExecutorBus().GetExecutor(executor_name);
      });

  core_ptr_->GetServiceDispatcher().RegisterRpcDriver(
      std::move(dds_rpc_driver_ptr));
}

}  // namespace ibmw::extensions::dds_extension

IBMW_EXT_ENTRY(ibmw::extensions::dds_extension::DdsExtension)
