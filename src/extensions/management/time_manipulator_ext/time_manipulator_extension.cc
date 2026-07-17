// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "time_manipulator_ext/time_manipulator_extension.h"

#include "cpp/service/rpc_handle.h"
#include "engine/engine.h"
#include "time_manipulator_ext/global.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::time_manipulator_extension::TimeManipulatorExtension::Options> {
  using Options = ibmw::extensions::time_manipulator_extension::TimeManipulatorExtension::Options;

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

namespace ibmw::extensions::time_manipulator_extension {

TimeManipulatorExtension::TimeManipulatorExtension() {
  SetExtensionLogger(&g_logger);
}

bool TimeManipulatorExtension::OnInit(runtime::core::Engine* core_ptr) {
  YAML::Node extension_options_node = core_ptr_->GetExtensionBus().GetExtensionOptionsNode(Name());

  if (extension_options_node && !extension_options_node.IsNull()) {
    options_ = extension_options_node.as<Options>();
  }

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kExecutor),
                          [this] { RegisterTimeManipulatorExecutor(); });

  core_ptr_->RegisterHook(runtime::core::Before(runtime::core::Phase::kInitialize,
                                                 runtime::core::Subsystem::kModule),
                          [this] { RegisterRpcService(); });

  extension_options_node = options_;
  core_ptr_->GetExtensionBus().UpdateExtensionOptionsNode(Name(), extension_options_node);

  return true;
}

void TimeManipulatorExtension::OnShutdown() {
}

void TimeManipulatorExtension::RegisterTimeManipulatorExecutor() {
  core_ptr_->GetExecutorBus().RegisterExecutorGenFunc(
      "time_manipulator",
      [this]() -> std::unique_ptr<ibmw::runtime::core::executor::ExecutorBase> {
        auto ptr = std::make_unique<TimeManipulatorExecutor>();
        ptr->RegisterGetExecutorFunc(
            [this](std::string_view executor_name) -> ibmw::executor::ExecutorHandle {
              return core_ptr_->GetExecutorBus().GetExecutor(executor_name);
            });
        return ptr;
      });
}

void TimeManipulatorExtension::RegisterRpcService() {
  service_ptr_ = std::make_unique<TimeManipulatorServiceImpl>();

  if (!options_.service_name.empty())
    service_ptr_->SetServiceName(options_.service_name);

  const auto& executor_vec = core_ptr_->GetExecutorBus().GetAllExecutors();

  for (const auto& itr : executor_vec) {
    if (itr->Type() == "time_manipulator") {
      auto* ptr = dynamic_cast<TimeManipulatorExecutor*>(itr.get());
      if (ptr) {
        service_ptr_->RegisterTimeManipulatorExecutor(ptr);
      } else {
        MW_ERROR("Invalid executor pointer");
      }
    }
  }

  auto service_ref = ibmw::rpc::ServiceRef(
      core_ptr_->GetServiceDispatcher().GetRpcAccess().NativeHandle());

  bool ret = service_ref.RegisterService(service_ptr_.get());
  MW_CHECK_ERROR(ret, "Register service failed.");
}

}  // namespace ibmw::extensions::time_manipulator_extension

IBMW_EXT_ENTRY(ibmw::extensions::time_manipulator_extension::TimeManipulatorExtension)
