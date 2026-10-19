// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include "engine/transport/transport_bus.h"

namespace ibmw::runtime::core::transport {
namespace {

class SerializedAbiDriver final : public TransportDriver {
 public:
  std::string_view Name() const noexcept override { return "serialized-abi"; }
  IBMW_DECLARE_DRIVER_SLOT("serialized-abi")
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterSendType(const SendTypeWrapper&) noexcept override { return true; }
  bool OnMessage(const MessageHandlerWrapper&) noexcept override { return true; }
  void Send(MessageFrame&) noexcept override {}
};

struct CaptureState {
  unsigned calls = 0;
  std::string wire_topic;
  std::string wire_type;
};

SerializedTransportCapabilityV2 Capability(CaptureState& state) {
  return {
      .capture_mode = SerializedCaptureMode::kNativeSerialized,
      .serialization = SerializedFormatSupport::kNative,
      .provider_name = "serialized-abi",
      .operations = {
          .context = &state,
          .supports_topic = [](void*, const TopicDescriptor&) noexcept { return true; },
          .attach_subscriber = [](void* context, const SerializedSubscriberOptionsV2& options,
                                  SerializedMessageCallback) noexcept {
            auto& state = *static_cast<CaptureState*>(context);
            ++state.calls;
            state.wire_topic = options.wire_topic_name;
            state.wire_type = options.wire_type_name;
            return SerializedSubscriberHandle{1};
          },
          .detach_subscriber = [](void*, SerializedSubscriberHandle) noexcept { return true; }}};
}

TEST(SerializedTransportAbi, RejectsOldFutureAndTruncatedContractsBeforeRegistration) {
  for (unsigned invalid = 0; invalid < 3; ++invalid) {
    CaptureState state;
    TransportBus bus;
    auto capability = Capability(state);
    if (invalid == 0) capability.abi_version = 1;
    if (invalid == 1) capability.abi_version = 3;
    if (invalid == 2) capability.struct_size = sizeof(capability) - 1;
    std::unique_ptr<TransportDriver> driver = std::make_unique<SerializedAbiDriver>();
    auto* identity = driver.get();
    EXPECT_THROW(bus.RegisterTransportDriver(std::move(driver), {}, capability),
                 ibmw::common::util::IbmwException);
    ASSERT_NE(driver, nullptr);
    EXPECT_EQ(bus.GetSerializedTransportCapability(*identity), nullptr);
    EXPECT_EQ(state.calls, 0u);
  }
}

TEST(SerializedTransportAbi, MatchingContractPreservesNativeWireAddress) {
  CaptureState state;
  TransportBus bus;
  std::unique_ptr<TransportDriver> driver = std::make_unique<SerializedAbiDriver>();
  auto* identity = driver.get();
  bus.RegisterTransportDriver(std::move(driver), {}, Capability(state));
  const auto* capability = bus.GetSerializedTransportCapability(*identity);
  ASSERT_NE(capability, nullptr);
  ASSERT_TRUE(capability->HasValidContract());
  const SerializedSubscriberOptionsV2 options{
      .topic = {.topic = "/logical", .msg_type = "logical.Type"},
      .wire_topic_name = "native/topic",
      .wire_type_name = "native.Type"};
  EXPECT_TRUE(capability->operations.attach_subscriber(
      capability->operations.context, options, [](auto&&) {}));
  EXPECT_EQ(state.calls, 1u);
  EXPECT_EQ(state.wire_topic, "native/topic");
  EXPECT_EQ(state.wire_type, "native.Type");
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
