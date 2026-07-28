// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "engine/transport/serialized_publisher.h"
#include "engine/transport/serialized_subscriber.h"

namespace ibmw::runtime::core::transport {

enum class SerializedCaptureMode : uint8_t {
  kRuntimeTap = 0,
  kNativeSerialized = 1,
  kCachedSerialized = 2,
};

enum class SerializedPublishMode : uint8_t {
  kBus = 0,
  kNativeSerialized = 1,
};

enum class SerializedFormatSupport : uint8_t {
  kTypeSupport = 0,
  kNative = 1,
};

enum class SerializedSchemaSupport : uint8_t {
  kTypeSupportPackage = 0,
  kRuntimeAdvertisement = 1,
};

enum class SerializedLoanSupport : uint8_t {
  kNone = 0,
  // The callback may retain a transport-owned payload through shared
  // ownership; the transport does not reuse it until the last owner releases.
  kSharedPayload = 1,
};

enum class SerializedBackpressure : uint8_t {
  // Queueing, blocking, and loss follow the transport QoS/configuration.
  kTransportManaged = 0,
  // Send returns false rather than blocking when the transport cannot accept.
  kRejectOnPressure = 1,
};

enum class SerializedCallbackThread : uint8_t {
  kTransportManaged = 0,
  kConfiguredExecutor = 1,
};

enum class SerializedDetachSemantics : uint8_t {
  kQuiescentOnReturn = 0,
};

enum class SerializedPublishCompletion : uint8_t {
  kPayloadConsumedOnReturn = 0,
};

struct SerializedTransportOperationsV1 {
  // Borrowed driver-owned context. It has the same lifetime as the registered
  // TransportDriver and is never released through this table.
  void* context = nullptr;

  // Query operations are side-effect free and may be called during setup.
  bool (*supports_topic)(
      void* context, const TopicDescriptor& topic) noexcept = nullptr;

  // A successful attach transfers callback ownership to the transport. Each
  // callback receives a SerializedMessage whose shared payload remains valid
  // after the callback returns. The provider fills MessageIdentity/source
  // fields when the transport exposes them; unavailable identity stays
  // invalid. Callbacks run on the declared callback thread.
  SerializedSubscriberHandle (*attach_subscriber)(
      void* context, const SerializedSubscriberOptions& options,
      SerializedMessageCallback callback) noexcept = nullptr;

  // Detach is called by the lifecycle owner, not from the callback itself. A
  // true result means the callback is detached and quiescent before return.
  bool (*detach_subscriber)(
      void* context, SerializedSubscriberHandle handle) noexcept = nullptr;

  // Register returns zero on rejection. Send consumes/copies all borrowed
  // payload views before return, returns false on rejection/error, and does
  // not imply remote acknowledgment. Backpressure follows `backpressure`.
  SerializedPublisherHandle (*register_publisher)(
      void* context, const SerializedPublisherOptions& options) noexcept =
      nullptr;
  bool (*send)(
      void* context, SerializedPublisherHandle handle,
      const SerializedMessage& message) noexcept = nullptr;
  // When supports_ack is true, this waits for every native publisher created
  // through this context. True means all applicable remote acknowledgments
  // arrived before timeout; false covers timeout and transport error.
  bool (*wait_for_acknowledgments)(
      void* context, std::chrono::milliseconds timeout) noexcept = nullptr;

  // QoS query callbacks are optional but must be supplied as a pair. Returned
  // descriptors own their strings and remain independent of the driver.
  QosDescriptor (*publisher_qos)(
      void* context, std::string_view topic,
      std::string_view msg_type) noexcept = nullptr;
  QosDescriptor (*subscriber_qos)(
      void* context, std::string_view topic,
      std::string_view msg_type) noexcept = nullptr;
};

/**
 * @brief Versioned serialized transport capability slot.
 *
 * `provider_name` is the extension/provider identity supplied by the
 * registering extension; it is not inferred from `TransportDriver::Name()`.
 * All pointers are borrowed and remain valid for the registered driver
 * lifetime.  `struct_size` allows a future V2 to append fields without
 * changing the V1 consumer's layout assumptions. This slot follows the
 * repository's current C++ extension ABI policy; it does not claim binary
 * compatibility across unrelated compiler or standard-library toolchains.
 */
struct SerializedTransportCapabilityV1 {
  static constexpr uint32_t kAbiVersion = 1;

  uint32_t abi_version = kAbiVersion;
  uint32_t struct_size = sizeof(SerializedTransportCapabilityV1);
  SerializedCaptureMode capture_mode = SerializedCaptureMode::kRuntimeTap;
  SerializedPublishMode publish_mode = SerializedPublishMode::kBus;
  SerializedFormatSupport serialization =
      SerializedFormatSupport::kTypeSupport;
  SerializedSchemaSupport schema =
      SerializedSchemaSupport::kTypeSupportPackage;
  SerializedLoanSupport loan = SerializedLoanSupport::kNone;
  SerializedBackpressure backpressure =
      SerializedBackpressure::kTransportManaged;
  SerializedCallbackThread callback_thread =
      SerializedCallbackThread::kTransportManaged;
  SerializedDetachSemantics detach =
      SerializedDetachSemantics::kQuiescentOnReturn;
  SerializedPublishCompletion publish_completion =
      SerializedPublishCompletion::kPayloadConsumedOnReturn;
  uint32_t capture_priority = 100;
  bool supports_ack = false;
  std::string_view provider_name;
  std::string_view provider_instance_option;
  std::string_view provider_clear_option;
  std::string_view provider_strip_leading_slash_option;
  SerializedTransportOperationsV1 operations;

  bool IsUsable() const noexcept {
    return abi_version == kAbiVersion &&
           struct_size >= sizeof(SerializedTransportCapabilityV1);
  }

  bool HasValidContract() const noexcept {
    const bool valid_capture_mode =
        capture_mode == SerializedCaptureMode::kRuntimeTap ||
        capture_mode == SerializedCaptureMode::kNativeSerialized ||
        capture_mode == SerializedCaptureMode::kCachedSerialized;
    const bool valid_publish_mode =
        publish_mode == SerializedPublishMode::kBus ||
        publish_mode == SerializedPublishMode::kNativeSerialized;
    const bool valid_serialization =
        serialization == SerializedFormatSupport::kTypeSupport ||
        serialization == SerializedFormatSupport::kNative;
    const bool valid_schema =
        schema == SerializedSchemaSupport::kTypeSupportPackage ||
        schema == SerializedSchemaSupport::kRuntimeAdvertisement;
    const bool valid_loan =
        loan == SerializedLoanSupport::kNone ||
        loan == SerializedLoanSupport::kSharedPayload;
    const bool valid_backpressure =
        backpressure == SerializedBackpressure::kTransportManaged ||
        backpressure == SerializedBackpressure::kRejectOnPressure;
    const bool valid_callback_thread =
        callback_thread == SerializedCallbackThread::kTransportManaged ||
        callback_thread == SerializedCallbackThread::kConfiguredExecutor;
    const bool valid_detach =
        detach == SerializedDetachSemantics::kQuiescentOnReturn;
    const bool valid_publish_completion =
        publish_completion ==
        SerializedPublishCompletion::kPayloadConsumedOnReturn;
    const bool native_capture =
        capture_mode == SerializedCaptureMode::kNativeSerialized;
    const bool direct_capture =
        native_capture ||
        capture_mode == SerializedCaptureMode::kCachedSerialized;
    const bool native_publish =
        publish_mode == SerializedPublishMode::kNativeSerialized;
    const bool valid_capture_operations =
        !native_capture ||
        (operations.context != nullptr &&
         operations.supports_topic != nullptr &&
         operations.attach_subscriber != nullptr &&
         operations.detach_subscriber != nullptr);
    const bool valid_publish_operations =
        !native_publish ||
        (operations.context != nullptr &&
         operations.supports_topic != nullptr &&
         operations.register_publisher != nullptr &&
         operations.send != nullptr);
    return IsUsable() && valid_capture_mode && valid_publish_mode &&
           valid_serialization && valid_schema && valid_loan &&
           valid_backpressure && valid_callback_thread && valid_detach &&
           valid_publish_completion &&
           (!direct_capture || !provider_name.empty()) &&
           valid_capture_operations && valid_publish_operations &&
           (!native_capture ||
            serialization == SerializedFormatSupport::kNative) &&
           (!supports_ack ||
            (native_publish &&
             operations.wait_for_acknowledgments != nullptr)) &&
           ((operations.publisher_qos == nullptr) ==
            (operations.subscriber_qos == nullptr));
  }
};

/** Legacy C++ capability interface implemented by native serialized drivers. */
class __attribute__((visibility("default"))) SerializedTransportDriver {
 public:
  virtual ~SerializedTransportDriver() = default;
  virtual SerializedSubscriberHandle AttachSerializedSubscriber(
      const SerializedSubscriberOptions&, SerializedMessageCallback) noexcept = 0;
  virtual bool DetachSerializedSubscriber(SerializedSubscriberHandle) noexcept = 0;
  virtual SerializedPublisherHandle RegisterSerializedPublisher(
      const SerializedPublisherOptions&) noexcept = 0;
  virtual bool SendSerialized(SerializedPublisherHandle,
                              const SerializedMessage&) noexcept = 0;
  virtual bool WaitForSerializedAcknowledgments(
      std::chrono::milliseconds) noexcept { return true; }
};

}  // namespace ibmw::runtime::core::transport
