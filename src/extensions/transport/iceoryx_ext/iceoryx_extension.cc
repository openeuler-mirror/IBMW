// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "iceoryx_ext/iceoryx_extension.h"
#include "iceoryx_ext/global.h"
#include "iceoryx_ext/iceoryx_transport_driver.h"
#include <unistd.h>

namespace YAML {
template <>
struct convert<ibmw::extensions::iceoryx_extension::IceoryxExtension::Options> {
  using Options = ibmw::extensions::iceoryx_extension::IceoryxExtension::Options;

  static Node encode(const Options &rhs) {
    Node node;

    node["shm_init_size"] = rhs.shm_init_size;
    node["runtime_id"] = rhs.runtime_id;

    return node;
  }

  static bool decode(const Node &node, Options &rhs) {
    if (!node.IsMap()) return false;

    if (node["shm_init_size"])
      rhs.shm_init_size = node["shm_init_size"].as<uint64_t>();

    if (node["runtime_id"].IsDefined()) {
      rhs.runtime_id = node["runtime_id"].as<std::string>();
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::iceoryx_extension {

IceoryxExtension::IceoryxExtension() {
  SetExtensionLogger(&g_logger);
}

bool IceoryxExtension::OnInit(runtime::core::Engine *core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }

  std::string runtime_id = "iceoryx" + std::to_string(getpid());

  if (!options_.runtime_id.empty()) {
    runtime_id = options_.runtime_id;
  }

  iceoryx_bus_.Initialize(options_.shm_init_size, runtime_id);

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kTransport),
                          [this] { RegisterIceoryxTransportDriver(); });

  extension_options_node = options_;

  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void IceoryxExtension::OnShutdown() {
  stop_flag_ = true;

  iceoryx_bus_.Shutdown();
}

void IceoryxExtension::RegisterIceoryxTransportDriver() {
  auto concrete_driver = std::make_unique<IceoryxTransportDriver>(iceoryx_bus_);
  auto* concrete_ptr = concrete_driver.get();
  std::unique_ptr<runtime::core::transport::TransportDriver>
      iceoryx_transport_driver_ptr = std::move(concrete_driver);

  core_ptr_->GetTransportBus().RegisterTransportDriver(
      std::move(iceoryx_transport_driver_ptr),
      runtime::core::transport::TransportDriverCapabilitiesV1{
          .shutdown_v1 = static_cast<
              runtime::core::transport::TransportShutdownCapabilityV1*>(
              concrete_ptr),
          .loaned_pod_registration_v1 = static_cast<
              runtime::core::transport::LoanedPodRegistrationCapabilityV1*>(
              concrete_ptr),
          .retained_loan_v1 = static_cast<
              runtime::core::transport::RetainedLoanCapabilityV1*>(
              concrete_ptr)},
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

}  // namespace ibmw::extensions::iceoryx_extension

IBMW_EXT_ENTRY(ibmw::extensions::iceoryx_extension::IceoryxExtension)
