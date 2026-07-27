#include "transport_driver.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace {

using ibmw::runtime::core::transport::LoanMode;
using ibmw::runtime::core::transport::LoanedMessageHandlerWrapper;
using ibmw::runtime::core::transport::LoanedSampleHandle;
using ibmw::runtime::core::transport::MessageFrame;
using ibmw::runtime::core::transport::MessageHandlerWrapper;
using ibmw::runtime::core::transport::SendTypeWrapper;
using ibmw::runtime::core::transport::TransportCatalog;
using ibmw::runtime::core::transport::TransportDriver;
using ibmw::runtime::core::transport::TransportInfo;

constexpr size_t kSlotCount = 15;
std::array<uint64_t, kSlotCount> g_calls{};
std::array<uint64_t, kSlotCount> g_results{};

enum Slot : size_t {
  kName,
  kDriverSlot,
  kInitialize,
  kStart,
  kShutdown,
  kReport,
  kCatalog,
  kRegisterSendType,
  kOnMessage,
  kAttachLoaned,
  kSend,
  kLoanSample,
  kLoanSerializedSample,
  kPublishLoaned,
  kReleaseLoaned,
};

void Record(Slot slot, uint64_t result) noexcept {
  ++g_calls[slot];
  g_results[slot] = result;
}

class LegacyTransportDriver final : public TransportDriver {
 public:
  std::string_view Name() const noexcept override {
    Record(kName, 0x101);
    return "legacy-transport-driver-v1";
  }

  ibmw_driver_slot_t Slot() const noexcept override {
    Record(kDriverSlot, 0xab100001U);
    return 0xab100001U;
  }

  void Initialize(YAML::Node options_node) override {
    Record(kInitialize,
           options_node["fixture"].as<std::string>() == "legacy" ? 0x103 : 0);
  }

  void Start() override { Record(kStart, 0x104); }
  void Shutdown() override { Record(kShutdown, 0x105); }

  std::list<std::pair<std::string, std::string>>
  GenInitializationReport() const noexcept override {
    Record(kReport, 0x106);
    return {{"abi-direction", "old-driver/new-core"}};
  }

  void SetTransportCatalog(const TransportCatalog* catalog) noexcept override {
    Record(kCatalog, reinterpret_cast<uintptr_t>(catalog));
  }

  bool RegisterSendType(const SendTypeWrapper& wrapper) noexcept override {
    Record(kRegisterSendType, reinterpret_cast<uintptr_t>(&wrapper));
    return true;
  }

  bool OnMessage(const MessageHandlerWrapper& wrapper) noexcept override {
    Record(kOnMessage, reinterpret_cast<uintptr_t>(&wrapper));
    return false;
  }

  bool AttachLoaned(
      const LoanedMessageHandlerWrapper& wrapper) noexcept override {
    Record(kAttachLoaned, reinterpret_cast<uintptr_t>(&wrapper));
    return true;
  }

  void Send(MessageFrame& frame) noexcept override {
    Record(kSend, reinterpret_cast<uintptr_t>(&frame));
  }

  bool LoanSample(const TransportInfo& info, size_t requested_size,
                  LoanMode mode, const ibmw::transport::Context* ctx,
                  LoanedSampleHandle& out_handle) noexcept override {
    const auto mode_value = static_cast<uint8_t>(mode);
    Record(kLoanSample, requested_size ^ (static_cast<uint64_t>(mode_value) << 32) ^
                            reinterpret_cast<uintptr_t>(&info) ^
                            reinterpret_cast<uintptr_t>(ctx) ^
                            reinterpret_cast<uintptr_t>(&out_handle));
    return true;
  }

  bool LoanSerializedSample(
      const TransportInfo& info, const ibmw::transport::Context& ctx,
      const void* msg_ptr, LoanedSampleHandle& out_handle) noexcept override {
    Record(kLoanSerializedSample,
           reinterpret_cast<uintptr_t>(&info) ^
               reinterpret_cast<uintptr_t>(&ctx) ^
               reinterpret_cast<uintptr_t>(msg_ptr) ^
               reinterpret_cast<uintptr_t>(&out_handle));
    return true;
  }

  void PublishLoaned(LoanedSampleHandle& handle) noexcept override {
    Record(kPublishLoaned, reinterpret_cast<uintptr_t>(&handle));
  }

  void ReleaseLoaned(LoanedSampleHandle& handle) noexcept override {
    Record(kReleaseLoaned, reinterpret_cast<uintptr_t>(&handle));
  }
};

}  // namespace

#if defined(_WIN32)
#define LEGACY_FIXTURE_EXPORT __declspec(dllexport)
#else
#define LEGACY_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" LEGACY_FIXTURE_EXPORT void* legacy_transport_driver_create() {
  g_calls.fill(0);
  g_results.fill(0);
  return new LegacyTransportDriver;
}

extern "C" LEGACY_FIXTURE_EXPORT void legacy_transport_driver_destroy(
    void* driver) {
  delete static_cast<LegacyTransportDriver*>(driver);
}

extern "C" LEGACY_FIXTURE_EXPORT uint64_t
legacy_transport_driver_call_count(uint32_t slot) {
  return slot < g_calls.size() ? g_calls[slot] : 0;
}

extern "C" LEGACY_FIXTURE_EXPORT uint64_t legacy_transport_driver_result(
    uint32_t slot) {
  return slot < g_results.size() ? g_results[slot] : 0;
}
