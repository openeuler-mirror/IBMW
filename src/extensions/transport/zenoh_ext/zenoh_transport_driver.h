// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "engine/transport/serialized_callback.h"
#include "engine/transport/serialized_transport_driver.h"
#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_tools.h"
#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "zenoh.h"
#include "zenoh_ext/key_expr.h"
#include "zenoh_ext/util.h"
#include "zenoh_ext/zenoh_buffer_array_allocator.h"
#include "zenoh_ext/zenoh_bus.h"

namespace ibmw::extensions::zenoh_extension {

class ZenohTransportDriver : public runtime::core::transport::TransportDriver {
 public:
  struct Options {
    struct PubTopicOptions {
      std::string topic_name;
      bool shm_enabled = false;
    };
    std::vector<PubTopicOptions> pub_topics_options;
  };

 public:
  ZenohTransportDriver(
      const std::shared_ptr<ZenohBus>& zenoh_util_ptr, const std::string& limit_domain, size_t shm_init_loan_size)
      : zenoh_bus_ptr_(zenoh_util_ptr),
        limit_domain_(NormalizeLimitDomain(limit_domain)),
        key_suffix_(LimitDomainKeySuffix(limit_domain_)),
        shm_init_loan_size_(shm_init_loan_size) {}

  ~ZenohTransportDriver() override = default;

  std::string_view Name() const noexcept override { return "zenoh"; }
  IBMW_DECLARE_DRIVER_SLOT("zenoh")

  void Initialize(YAML::Node options_node) override;
  void Start() override;
  void Shutdown() override;

  void SetTransportCatalog(const runtime::core::transport::TransportCatalog* transport_catalog_ptr) noexcept override {
    transport_catalog_ptr_ = transport_catalog_ptr;
  }

  bool RegisterSendType(
      const runtime::core::transport::SendTypeWrapper& send_type_wrapper) noexcept override;
  bool OnMessage(const runtime::core::transport::MessageHandlerWrapper& message_handler) noexcept override;
  void Send(runtime::core::transport::MessageFrame& msg_wrapper) noexcept override;

  runtime::core::transport::SerializedTransportOperationsV2 SerializedOperations() noexcept;
  runtime::core::transport::SerializedSubscriberHandle AttachSerializedSubscriber(
      const runtime::core::transport::SerializedSubscriberOptionsV2& options,
      runtime::core::transport::SerializedMessageCallback callback) noexcept;
  bool DetachSerializedSubscriber(runtime::core::transport::SerializedSubscriberHandle handle) noexcept;

 private:
  struct NativeSubscriber {
    z_owned_subscriber_t subscription;
    std::shared_ptr<runtime::core::transport::SerializedCallback> callback;
    NativeSubscriber() { z_internal_null(&subscription); }
    ~NativeSubscriber() {
      if (callback) callback->Stop();
      z_drop(z_move(subscription));
    }
  };
  std::mutex native_mutex_;
  uint64_t next_native_handle_ = 1;
  std::unordered_map<uint64_t, std::unique_ptr<NativeSubscriber>> native_subscribers_;

  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  Options options_;
  std::atomic<State> state_ = State::kPreInit;

  const runtime::core::transport::TransportCatalog* transport_catalog_ptr_ = nullptr;

  std::shared_ptr<ZenohBus> zenoh_bus_ptr_;

  std::string limit_domain_;
  std::string key_suffix_;

  std::shared_mutex z_shared_mutex_;

  std::unordered_map<
      std::string,
      std::unique_ptr<ibmw::runtime::core::transport::MessageHandlerTool>>
      message_handler_map_;

  std::unordered_map<std::string, uint64_t> z_pub_shm_size_map_;

  uint64_t shm_init_loan_size_;
};

}  // namespace ibmw::extensions::zenoh_extension
