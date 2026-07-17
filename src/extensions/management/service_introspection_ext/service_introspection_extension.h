// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>

#include "engine/extension/extension_base.h"
#include "cpp/transport/transport_handle.h"

namespace ibmw::extensions::service_introspection_extension {

class ServiceIntrospectionExtension : public ExtensionBase {
 public:
  struct Options {
    enum class Mode {
      kMeta,
      khybrid,
      kFull
    };

    Mode mode = Mode::kFull;

    enum class RpcSerializationType {
      kJson,
      kAuto,
    };
    RpcSerializationType rpc_serialization_type = RpcSerializationType::kJson;

    std::string client_info_topic_name;
    std::string server_info_topic_name;
  };

 public:
  ServiceIntrospectionExtension();
  ~ServiceIntrospectionExtension() override = default;

  std::string_view Name() const noexcept override { return "service_introspection_extension"; }

  bool OnInit(runtime::core::Engine *core_ptr) override;
  void OnShutdown() override;

 private:
  void RegisterPublisher();
  void RegisterClientFilter();
  void RegisterServiceFilter();

 private:
  Options options_;

  ibmw::transport::SenderRef publisher_;
  ibmw::transport::SenderRef publisher2_;

  std::unique_ptr<ibmw::transport::SenderRef> client_info_publisher_;
  std::unique_ptr<ibmw::transport::SenderRef> server_info_publisher_;

  std::atomic_bool stop_flag_ = false;
};

}  // namespace ibmw::extensions::service_introspection_extension
