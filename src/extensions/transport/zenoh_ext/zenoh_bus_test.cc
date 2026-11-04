// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// TRANS-046: the publisher catalog changes only before the bus is frozen, the
// shared put options are set once, and RPC node registration reports failure.
// One isolated session (no scouting, loopback only).

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "zenoh_ext/zenoh_bus.h"

namespace ibmw::extensions::zenoh_extension {
namespace {

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

}  // namespace
}  // namespace ibmw::extensions::zenoh_extension
