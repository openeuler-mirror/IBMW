// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "iceoryx_ext/iceoryx_extension.h"
#include "iceoryx_ext/global.h"
#include "iceoryx_ext/iceoryx_transport_driver.h"
#if defined(_WIN32)
  #include <windows.h>
#else
  #include <unistd.h>
#endif

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

#if defined(_WIN32)
  std::string runtime_id = "iceoryx" + std::to_string(GetProcessId(GetCurrentProcess()));
#else
  std::string runtime_id = "iceoryx" + std::to_string(getpid());
#endif

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
  std::unique_ptr<runtime::core::transport::TransportDriver> iceoryx_transport_driver_ptr =
      std::make_unique<IceoryxTransportDriver>(iceoryx_bus_);

  core_ptr_->GetTransportBus().RegisterTransportDriver(std::move(iceoryx_transport_driver_ptr));
}

}  // namespace ibmw::extensions::iceoryx_extension

IBMW_EXT_ENTRY(ibmw::extensions::iceoryx_extension::IceoryxExtension)
