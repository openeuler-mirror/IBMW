// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// TRANS-046: the publisher catalog changes only before the bus is frozen, the
// shared put options are set once, and RPC node registration reports failure.
// Removing a subscriber waits for its running callbacks.
// One isolated session (no scouting, loopback only).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>

#include "zenoh_ext/zenoh_bus.h"

namespace ibmw::extensions::zenoh_extension {
namespace {

using namespace std::chrono_literals;

class ZenohBusTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char dir[] = "/tmp/ibmw_zenoh_bus_test_XXXXXX";
    ASSERT_NE(::mkdtemp(dir), nullptr);
    directory_ = dir;
    std::ofstream(directory_ / "peer.json5")
        << R"({"mode":"peer","scouting":{"multicast":{"enabled":false},"gossip":{"enabled":false}},)"
        << R"("listen":{"endpoints":["tcp/127.0.0.1:0"]}})";
    bus_.Initialize((directory_ / "peer.json5").string(), 1024 * 1024);
  }

  void TearDown() override {
    bus_.Shutdown();
    std::filesystem::remove_all(directory_);
  }

  static void Ignore(const z_loaned_sample_t*) {}

  void Put(const std::string& keyexpr) {
    char byte = 0;
    bus_.Send(keyexpr, &byte, 1);
  }

  std::filesystem::path directory_;
  ZenohBus bus_;
};

TEST_F(ZenohBusTest, RegistrationIsRefusedAfterFreeze) {
  ASSERT_TRUE(bus_.RegisterPublisher("test/early", false));
  bus_.Freeze();
  EXPECT_FALSE(bus_.RegisterPublisher("test/late", false));
  EXPECT_FALSE(bus_.RegisterSubscriber("test/late", Ignore));
  EXPECT_FALSE(bus_.RegisterRpcNode("test/f", Ignore, "server", false));
  EXPECT_EQ(bus_.GetPublisherRegisterMap().size(), 1u);
  z_owned_subscriber_t capture;  // native capture attaches at run time
  ASSERT_TRUE(bus_.CreateSubscriber("test/capture", Ignore, capture));
  z_drop(z_move(capture));
}

TEST_F(ZenohBusTest, ShutdownEndsTheFreeze) {
  bus_.Freeze();
  bus_.Shutdown();
  bus_.Initialize((directory_ / "peer.json5").string(), 1024 * 1024);
  EXPECT_TRUE(bus_.RegisterPublisher("test/a", false));
}

// Registering a publisher does not rewrite the options all puts share.
TEST_F(ZenohBusTest, PutOptionsAreSetOnce) {
  EXPECT_EQ(bus_.z_pub_options_.attachment, nullptr);
  z_timestamp_t timestamp{};
  bus_.z_pub_options_.timestamp = &timestamp;
  ASSERT_TRUE(bus_.RegisterPublisher("test/a", false));
  EXPECT_EQ(bus_.z_pub_options_.timestamp, &timestamp);
  bus_.z_pub_options_.timestamp = nullptr;
}

TEST_F(ZenohBusTest, ClientsOfOneFunctionShareTheAnswerSubscriber) {
  EXPECT_TRUE(bus_.RegisterRpcNode("test/f", Ignore, "client", false));
  EXPECT_TRUE(bus_.RegisterRpcNode("test/f", Ignore, "client", false));
}

// A later server is accepted (the driver test shows it is never called).
TEST_F(ZenohBusTest, FunctionServerRegisteredAgainIsAccepted) {
  EXPECT_TRUE(bus_.RegisterRpcNode("test/f", Ignore, "server", false));
  EXPECT_TRUE(bus_.RegisterRpcNode("test/f", Ignore, "server", false));
}

TEST_F(ZenohBusTest, RpcNodeRegistrationReportsFailure) {
  EXPECT_FALSE(bus_.RegisterRpcNode("test/f", Ignore, "peer", false));
  EXPECT_FALSE(bus_.RegisterRpcNode("test//f", Ignore, "client", false));  // not a key expression
  EXPECT_TRUE(bus_.GetPublisherRegisterMap().empty());  // nothing reads it concurrently here
}

// Unregistering returns only once the running callback returned; no callback
// runs afterwards.
TEST_F(ZenohBusTest, UnregisterWaitsForTheRunningCallback) {
  std::atomic<int> calls = 0;
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  ASSERT_TRUE(bus_.RegisterSubscriber("test/u", [&](const z_loaned_sample_t*) {
    if (++calls == 1) {
      entered.set_value();
      released.wait();
    }
  }));
  ASSERT_TRUE(bus_.RegisterPublisher("test/u", false));
  bus_.Freeze();
  // A local subscriber runs in the put: the first one blocks its thread.
  auto first = std::async(std::launch::async, [&] { Put("test/u"); });
  ASSERT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
  auto unregister = std::async(std::launch::async, [&] { return bus_.UnregisterSubscriber("test/u"); });
  EXPECT_EQ(unregister.wait_for(200ms), std::future_status::timeout);
  release.set_value();
  ASSERT_EQ(unregister.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(unregister.get());
  first.get();
  Put("test/u");  // a local subscriber would run in the put
  EXPECT_EQ(calls.load(), 1);
  EXPECT_FALSE(bus_.UnregisterSubscriber("test/u"));
}

// A callback that removes subscribers (shutdown from a handler) is not waited
// for, which would never end.
TEST_F(ZenohBusTest, UnregisterFromACallbackDoesNotWaitForIt) {
  std::promise<bool> removed;
  ASSERT_TRUE(bus_.RegisterSubscriber("test/u", [&](const z_loaned_sample_t*) {
    removed.set_value(bus_.UnregisterSubscriber("test/u"));
  }));
  ASSERT_TRUE(bus_.RegisterPublisher("test/u", false));
  bus_.Freeze();
  auto result = removed.get_future();
  Put("test/u");
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(result.get());
}

// The same after the callback's own put ran another callback on its thread.
TEST_F(ZenohBusTest, UnregisterFromACallbackAfterANestedOneDoesNotWait) {
  std::promise<bool> removed;
  ASSERT_TRUE(bus_.RegisterSubscriber("test/inner", Ignore));
  ASSERT_TRUE(bus_.RegisterSubscriber("test/outer", [&](const z_loaned_sample_t*) {
    Put("test/inner");
    removed.set_value(bus_.UnregisterSubscriber("test/outer"));
  }));
  ASSERT_TRUE(bus_.RegisterPublisher("test/outer", false));
  ASSERT_TRUE(bus_.RegisterPublisher("test/inner", false));
  bus_.Freeze();
  auto result = removed.get_future();
  auto put = std::async(std::launch::async, [&] { Put("test/outer"); });
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(result.get());
}

// Shutdown, too, waits for a running callback.
TEST_F(ZenohBusTest, ShutdownWaitsForTheRunningCallback) {
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  ASSERT_TRUE(bus_.RegisterSubscriber("test/u", [&](const z_loaned_sample_t*) {
    entered.set_value();
    released.wait();
  }));
  ASSERT_TRUE(bus_.RegisterPublisher("test/u", false));
  bus_.Freeze();
  auto put = std::async(std::launch::async, [&] { Put("test/u"); });
  ASSERT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
  auto shutdown = std::async(std::launch::async, [&] { bus_.Shutdown(); });
  EXPECT_EQ(shutdown.wait_for(200ms), std::future_status::timeout);
  release.set_value();
  ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
}

// Only the string's bytes are read: here a view of "1" in "12".
TEST(ZenohBusConfigTest, PrettyConfigReadsOnlyTheStringBytes) {
  z_view_string_t text;
  ASSERT_EQ(z_view_string_from_substr(&text, "12", 1), Z_OK);
  EXPECT_EQ(ZenohBus::PrettyConfig(z_loan(text)), "1");
  ASSERT_EQ(z_view_string_from_str(&text, R"({"mode":"peer"})"), Z_OK);
  EXPECT_EQ(ZenohBus::PrettyConfig(z_loan(text)), "{\n    \"mode\" : \"peer\"\n}");
  ASSERT_EQ(z_view_string_from_str(&text, "x"), Z_OK);
  EXPECT_EQ(ZenohBus::PrettyConfig(z_loan(text)), "null");
  z_view_string_empty(&text);
  EXPECT_EQ(ZenohBus::PrettyConfig(z_loan(text)), "null");
}

TEST_F(ZenohBusTest, UnregisterOfAnUnknownSubscriberFails) {
  EXPECT_FALSE(bus_.UnregisterSubscriber("test/none"));
}

}  // namespace
}  // namespace ibmw::extensions::zenoh_extension
