// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "log_control_ext/log_control_extension.h"

#include "cpp/service/rpc_handle.h"
#include "engine/engine.h"
#include "log_control_ext/global.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::log_control_extension::LogControlExtension::Options> {
  using Options = ibmw::extensions::log_control_extension::LogControlExtension::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["service_name"] = rhs.service_name;

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["service_name"])
      rhs.service_name = node["service_name"].as<std::string>();

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::log_control_extension {

LogControlExtension::LogControlExtension() {
  SetExtensionLogger(&g_logger);
}

bool LogControlExtension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kModule),
                          [this] { RegisterRpcService(); });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void LogControlExtension::OnShutdown() {
}

void LogControlExtension::RegisterRpcService() {
  service_ptr_ = std::make_unique<LogControlServiceImpl>();

  if (!options_.service_name.empty())
    service_ptr_->SetServiceName(options_.service_name);

  service_ptr_->SetLoggingSubsystem(&(core_ptr_->GetLoggingSubsystem()));

  auto service_ref = ibmw::rpc::ServiceRef(
      core_ptr_->GetServiceDispatcher().GetRpcAccess().NativeHandle());

  bool ret = service_ref.RegisterService(service_ptr_.get());
  MW_CHECK_ERROR(ret, "Register service failed.");
}

}  // namespace ibmw::extensions::log_control_extension

IBMW_EXT_ENTRY(ibmw::extensions::log_control_extension::LogControlExtension)
