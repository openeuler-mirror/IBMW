// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the MQTT message catalog (TRANS-010/015): Paho
// buffers are always released, Shutdown waits for running handlers and
// never deadlocks when called from inside a handler.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>

#include "cpp/util/string.h"
#include "mqtt_ext/msg_handle_catalog.h"

namespace ibmw::extensions::mqtt_extension {
namespace {

using namespace std::chrono_literals;

// Allocates the message the way Paho does, so MQTTAsync_freeMessage and
// MQTTAsync_free can release it.
MQTTAsync_message* NewMessage() {
  auto* msg = static_cast<MQTTAsync_message*>(std::malloc(sizeof(MQTTAsync_message)));
  const MQTTAsync_message init = MQTTAsync_message_initializer;
  std::memcpy(msg, &init, sizeof(init));
  msg->payload = std::malloc(4);
  msg->payloadlen = 4;
  return msg;
}

char* NewTopic(const char* topic) {
  char* t = static_cast<char*>(std::malloc(std::strlen(topic) + 1));
  std::strcpy(t, topic);
  return t;
}

void Deliver(MsgHandleCatalog& catalog, const char* topic) {
  char* raw = NewTopic(topic);
  catalog.HandleServerMsg(raw, raw, NewMessage());
}

// Executor that runs every task synchronously inside Dispatch.
struct InlineExecutor {
  ibmw_executor_base_t base{
      .type = [](void*) { return ibmw::util::ToIbmwStringView("inline"); },
      .name = [](void*) { return ibmw::util::ToIbmwStringView("inline"); },
      .is_thread_safe = [](void*) { return true; },
      .is_in_current_executor = [](void*) { return true; },
      .supports_timed_dispatch = [](void*) { return false; },
      .dispatch = [](void*, ibmw_function_base_t* task) {
            executor::Task t(task);
            t();
          },
      .now = [](void*) -> uint64_t { return 0; },
      .dispatch_at_ns = nullptr,
      .ctx = nullptr};
};

TEST(MqttMsgHandleCatalog, UnknownTopicIsIgnored) {
  MsgHandleCatalog catalog;
  int calls = 0;
  catalog.RegisterMsgHandle("known", [&](MQTTAsync_message*) { ++calls; });
  Deliver(catalog, "unknown");
  Deliver(catalog, "known");
  EXPECT_EQ(calls, 1);
  catalog.Shutdown();
}

TEST(MqttMsgHandleCatalog, ThrowingHandlerIsContained) {
  MsgHandleCatalog catalog;
  catalog.RegisterMsgHandle("t", [](MQTTAsync_message*) { throw std::runtime_error("boom"); });
  EXPECT_NO_FATAL_FAILURE(Deliver(catalog, "t"));
  catalog.Shutdown();
}

TEST(MqttMsgHandleCatalog, ShutdownWaitsForRunningHandler) {
  MsgHandleCatalog catalog;
  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::atomic<bool> finished{false};
  catalog.RegisterMsgHandle("t", [&](MQTTAsync_message*) {
    entered.set_value();
    release_future.wait();
    finished = true;
  });

  std::thread paho([&] { Deliver(catalog, "t"); });
  entered.get_future().wait();

  auto shutdown = std::async(std::launch::async, [&] { catalog.Shutdown(); });
  EXPECT_EQ(shutdown.wait_for(200ms), std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(shutdown.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(finished.load());
  paho.join();
}

TEST(MqttMsgHandleCatalog, ShutdownFromInsideHandlerDoesNotDeadlock) {
  MsgHandleCatalog catalog;
  catalog.RegisterMsgHandle("t", [&](MQTTAsync_message*) { catalog.Shutdown(); });
  auto done = std::async(std::launch::async, [&] { Deliver(catalog, "t"); });
  EXPECT_EQ(done.wait_for(5s), std::future_status::ready);
}

TEST(MqttMsgHandleCatalog, ShutdownFromInsideInlineExecutorHandlerDoesNotDeadlock) {
  InlineExecutor inline_executor;
  MsgHandleCatalog catalog;
  catalog.RegisterGetExecutorFunc(
      [&](std::string_view) { return executor::ExecutorHandle(&inline_executor.base); });
  catalog.InitExecutor("inline");
  int calls = 0;
  catalog.RegisterMsgHandle("t", [&](MQTTAsync_message*) {
    ++calls;
    catalog.Shutdown();
  });
  auto done = std::async(std::launch::async, [&] { Deliver(catalog, "t"); });
  ASSERT_EQ(done.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(calls, 1);
}

TEST(MqttMsgHandleCatalog, MessagesAfterShutdownAreDropped) {
  MsgHandleCatalog catalog;
  int calls = 0;
  catalog.RegisterMsgHandle("t", [&](MQTTAsync_message*) { ++calls; });
  catalog.Shutdown();
  Deliver(catalog, "t");
  EXPECT_EQ(calls, 0);
}

}  // namespace
}  // namespace ibmw::extensions::mqtt_extension
