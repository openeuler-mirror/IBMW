// Frozen TransportDriver v1 ABI declaration. See README.md in this directory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <utility>

#include "yaml-cpp/yaml.h"

typedef uint64_t ibmw_driver_slot_t;

namespace ibmw::transport {
class Context;
}  // namespace ibmw::transport

namespace ibmw::runtime::core::transport {

class TransportCatalog;
struct SendTypeWrapper;
struct MessageHandlerWrapper;
struct LoanedMessageHandlerWrapper;
struct MessageFrame;
struct TransportInfo;
struct LoanedSampleHandle;
enum class LoanMode : uint8_t;

class TransportDriver {
 public:
  TransportDriver() = default;
  virtual ~TransportDriver() = default;

  TransportDriver(const TransportDriver&) = delete;
  TransportDriver& operator=(const TransportDriver&) = delete;

  virtual std::string_view Name() const noexcept = 0;
  virtual ibmw_driver_slot_t Slot() const noexcept = 0;

  virtual void Initialize(YAML::Node options_node) = 0;
  virtual void Start() = 0;
  virtual void Shutdown() = 0;

  virtual std::list<std::pair<std::string, std::string>>
  GenInitializationReport() const noexcept {
    return {};
  }

  virtual void SetTransportCatalog(
      const TransportCatalog* transport_catalog_ptr) noexcept {}

  virtual bool RegisterSendType(
      const SendTypeWrapper& send_type_wrapper) noexcept = 0;
  virtual bool OnMessage(
      const MessageHandlerWrapper& message_handler) noexcept = 0;

  virtual bool AttachLoaned(
      const LoanedMessageHandlerWrapper& /*message_handler*/) noexcept {
    return false;
  }

  virtual void Send(MessageFrame& msg_wrapper) noexcept = 0;

  virtual bool LoanSample(const TransportInfo& /*info*/,
                          size_t /*requested_size*/,
                          LoanMode /*mode*/,
                          const ibmw::transport::Context* /*ctx*/,
                          LoanedSampleHandle& /*out_handle*/) noexcept {
    return false;
  }

  virtual bool LoanSerializedSample(
      const TransportInfo& /*info*/,
      const ibmw::transport::Context& /*ctx*/,
      const void* /*msg_ptr*/,
      LoanedSampleHandle& /*out_handle*/) noexcept {
    return false;
  }

  virtual void PublishLoaned(LoanedSampleHandle& /*handle*/) noexcept {}
  virtual void ReleaseLoaned(LoanedSampleHandle& /*handle*/) noexcept {}
};

}  // namespace ibmw::runtime::core::transport
