// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "zenoh_ext/zenoh_extension.h"
#include "zenoh_ext/global.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::zenoh_extension::ZenohExtension::Options> {
  using Options = ibmw::extensions::zenoh_extension::ZenohExtension::Options;

  static Node encode(const Options &rhs) {
    Node node;

    node["native_cfg_path"] = rhs.native_cfg_path;
    node["limit_domain"] = rhs.limit_domain;
    node["shm_pool_size"] = rhs.shm_pool_size;
    node["shm_init_loan_size"] = rhs.shm_init_loan_size;

    return node;
  }

  static bool decode(const Node &node, Options &rhs) {
    if (!node.IsMap()) return false;

    if (node["native_cfg_path"])
      rhs.native_cfg_path = node["native_cfg_path"].as<std::string>();

    if (node["limit_domain"])
      rhs.limit_domain = '/' + node["limit_domain"].as<std::string>();

    if (node["shm_pool_size"])
      rhs.shm_pool_size = node["shm_pool_size"].as<size_t>();

    if (node["shm_init_loan_size"])
      rhs.shm_init_loan_size = node["shm_init_loan_size"].as<size_t>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::zenoh_extension {

ZenohExtension::ZenohExtension() {
  SetExtensionLogger(&g_logger);
}

bool ZenohExtension::OnInit(runtime::core::Engine *core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }

  // todo remove role
  zenoh_bus_ptr_->Initialize(options_.native_cfg_path, options_.shm_pool_size);

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kTransport),
                          [this] { RegisterZenohTransportDriver(); });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kService),
                          [this] { RegisterZenohRpcDriver(); });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void ZenohExtension::OnShutdown() {
  stop_flag_ = true;

  zenoh_bus_ptr_->Shutdown();
}

void ZenohExtension::RegisterZenohTransportDriver() {
  std::unique_ptr<runtime::core::transport::TransportDriver> zenoh_transport_driver_ptr =
      std::make_unique<ZenohTransportDriver>(zenoh_bus_ptr_, options_.limit_domain, options_.shm_init_loan_size);

  core_ptr_->GetTransportBus().RegisterTransportDriver(
      std::move(zenoh_transport_driver_ptr), {},
      runtime::core::transport::SerializedTransportCapabilityV1{
          .capture_mode = runtime::core::transport::SerializedCaptureMode::kCachedSerialized,
          .publish_mode = runtime::core::transport::SerializedPublishMode::kBus,
          .serialization = runtime::core::transport::SerializedFormatSupport::kTypeSupport,
          .schema = runtime::core::transport::SerializedSchemaSupport::kTypeSupportPackage,
          .loan = runtime::core::transport::SerializedLoanSupport::kSharedPayload,
          .callback_thread = runtime::core::transport::SerializedCallbackThread::kTransportManaged,
          .capture_priority = 2,
          .provider_name = Name(),
          .provider_strip_leading_slash_option = "limit_domain"});
}

void ZenohExtension::RegisterZenohRpcDriver() {
  std::unique_ptr<runtime::core::service::ServiceDriver> zenoh_rpc_driver_ptr =
      std::make_unique<ZenohRpcDriver>(zenoh_bus_ptr_, options_.limit_domain);

  static_cast<ZenohRpcDriver *>(zenoh_rpc_driver_ptr.get())
      ->RegisterGetExecutorFunc(
          [this](std::string_view executor_name) -> ibmw::executor::ExecutorHandle {
            return core_ptr_->GetExecutorBus().GetExecutor(executor_name);
          });

  core_ptr_->GetServiceDispatcher().RegisterRpcDriver(std::move(zenoh_rpc_driver_ptr));
}

}  // namespace ibmw::extensions::zenoh_extension

IBMW_EXT_ENTRY(ibmw::extensions::zenoh_extension::ZenohExtension)
