// This covers the old-driver/new-core direction only.
#include <dlfcn.h>

#include <cstdint>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "cpp/transport/transport_context.h"
#include "engine/transport/transport_driver.h"
#include "engine/transport/transport_driver_capabilities.h"

namespace ibmw::runtime::core::transport {
namespace {

struct LibraryCloser {
  void operator()(void* library) const noexcept { dlclose(library); }
};

template <typename Function>
Function LoadSymbol(void* library, const char* name) {
  dlerror();
  auto* symbol = dlsym(library, name);
  EXPECT_NE(symbol, nullptr) << name << ": " << dlerror();
  return reinterpret_cast<Function>(symbol);
}

TEST(LegacyTransportDriverAbi, OldDriverDispatchesEveryFrozenVirtualSlot) {
  void* raw_library = dlopen(LEGACY_TRANSPORT_DRIVER_FIXTURE_PATH,
                             RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(raw_library, nullptr) << dlerror();
  std::unique_ptr<void, LibraryCloser> library(raw_library);

  const auto create =
      LoadSymbol<void* (*)()>(library.get(), "legacy_transport_driver_create");
  const auto destroy = LoadSymbol<void (*)(void*)>(
      library.get(), "legacy_transport_driver_destroy");
  const auto call_count = LoadSymbol<uint64_t (*)(uint32_t)>(
      library.get(), "legacy_transport_driver_call_count");
  const auto result = LoadSymbol<uint64_t (*)(uint32_t)>(
      library.get(), "legacy_transport_driver_result");
  ASSERT_NE(create, nullptr);
  ASSERT_NE(destroy, nullptr);
  ASSERT_NE(call_count, nullptr);
  ASSERT_NE(result, nullptr);

  std::unique_ptr<TransportDriver, decltype(destroy)> driver(
      static_cast<TransportDriver*>(create()), destroy);
  ASSERT_NE(driver, nullptr);

  TransportCatalog catalog;
  SendTypeWrapper send_type{};
  MessageHandlerWrapper handler{};
  LoanedMessageHandlerWrapper loaned_handler{};
  TransportInfo info{};
  MessageFrame frame{.info = info};
  LoanedSampleHandle handle{};
  ibmw::transport::Context context;
  const uint64_t payload = 0xabcdef;

  EXPECT_EQ(driver->Name(), "legacy-transport-driver-v1");
  EXPECT_EQ(driver->Slot(), 0xab100001U);
  driver->Initialize(YAML::Load("fixture: legacy"));
  driver->Start();
  driver->Shutdown();
  EXPECT_EQ(driver->GenInitializationReport(),
            (std::list<std::pair<std::string, std::string>>{
                {"abi-direction", "old-driver/new-core"}}));
  driver->SetTransportCatalog(&catalog);
  EXPECT_TRUE(driver->RegisterSendType(send_type));
  EXPECT_FALSE(driver->OnMessage(handler));
  EXPECT_TRUE(driver->AttachLoaned(loaned_handler));
  driver->Send(frame);
  EXPECT_TRUE(driver->LoanSample(info, 4097, LoanMode::kCtxMetaThenPayload,
                                 &context, handle));
  EXPECT_TRUE(driver->LoanSerializedSample(info, context, &payload, handle));
  driver->PublishLoaned(handle);
  driver->ReleaseLoaned(handle);

  for (uint32_t slot = 0; slot < 15; ++slot) {
    EXPECT_EQ(call_count(slot), 1U) << "legacy vtable slot " << slot;
    EXPECT_NE(result(slot), 0U) << "legacy vtable slot " << slot;
  }

  EXPECT_EQ(result(6), reinterpret_cast<uintptr_t>(&catalog));
  EXPECT_EQ(result(7), reinterpret_cast<uintptr_t>(&send_type));
  EXPECT_EQ(result(8), reinterpret_cast<uintptr_t>(&handler));
  EXPECT_EQ(result(9), reinterpret_cast<uintptr_t>(&loaned_handler));
  EXPECT_EQ(result(10), reinterpret_cast<uintptr_t>(&frame));
  EXPECT_EQ(result(13), reinterpret_cast<uintptr_t>(&handle));
  EXPECT_EQ(result(14), reinterpret_cast<uintptr_t>(&handle));

  EXPECT_EQ(dynamic_cast<TransportShutdownCapabilityV1*>(driver.get()),
            nullptr);
  EXPECT_EQ(dynamic_cast<LoanedPodRegistrationCapabilityV1*>(driver.get()),
            nullptr);
  EXPECT_EQ(dynamic_cast<RetainedLoanCapabilityV1*>(driver.get()), nullptr);
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
