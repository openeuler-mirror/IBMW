// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the MQTT message catalog (TRANS-010/015): Paho
// buffers are always released, Shutdown waits for running handlers and
// never deadlocks when called from inside a handler.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

#include "cpp/util/string.h"
#include "mqtt_ext/msg_handle_catalog.h"

namespace ibmw::extensions::mqtt_extension {
namespace {

using namespace std::chrono_literals;

// Allocates with Paho's allocator, so MQTTAsync_freeMessage and
// MQTTAsync_free can release it.
MQTTAsync_message* NewMessage() {
  auto* msg = static_cast<MQTTAsync_message*>(MQTTAsync_malloc(sizeof(MQTTAsync_message)));
  const MQTTAsync_message init = MQTTAsync_message_initializer;
  std::memcpy(msg, &init, sizeof(init));
  msg->payload = MQTTAsync_malloc(4);
  msg->payloadlen = 4;
  return msg;
}

char* NewTopic(const char* topic) {
  char* t = static_cast<char*>(MQTTAsync_malloc(std::strlen(topic) + 1));
  std::strcpy(t, topic);
  return t;
}

void Deliver(MsgHandleCatalog& catalog, const char* topic) {
  char* raw = NewTopic(topic);
  catalog.HandleServerMsg(raw, raw, NewMessage());
}

// Executor that runs every task synchronously inside Dispatch and, like a
// strand, cannot tell whether the caller runs on it.
struct InlineExecutor {
  ibmw_executor_base_t base{
      .type = [](void*) { return ibmw::util::ToIbmwStringView("inline"); },
      .name = [](void*) { return ibmw::util::ToIbmwStringView("inline"); },
      .is_thread_safe = [](void*) { return true; },
      .is_in_current_executor = [](void*) { return false; },
      .supports_timed_dispatch = [](void*) { return false; },
      .dispatch = [](void*, ibmw_function_base_t* task) {
            executor::Task t(task);
            t();
          },
      .now = [](void*) -> uint64_t { return 0; },
      .dispatch_at_ns = nullptr,
      .ctx = nullptr};
};

// Single worker thread running queued tasks in order (a dedicated executor).
class SerialExecutor {
 public:
  SerialExecutor() {
    worker_ = std::thread([this] {
      std::unique_lock lock(mutex_);
      while (true) {
        cv_.wait(lock, [this] { return stop_ || (running_ && !tasks_.empty()); });
        if (tasks_.empty()) return;
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        lock.unlock();
        task();
        lock.lock();
      }
    });
  }
  ~SerialExecutor() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
      running_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }
  // Tasks are only run after Run().
  void Run() {
    {
      std::lock_guard lock(mutex_);
      running_ = true;
    }
    cv_.notify_all();
  }
  void Pause() {
    std::lock_guard lock(mutex_);
    running_ = false;
  }
  size_t Queued() {
    std::lock_guard lock(mutex_);
    return tasks_.size();
  }
  void PretendCurrent() { pretend_current_ = true; }
  executor::ExecutorHandle Handle() { return executor::ExecutorHandle(&base_); }

 private:
  static SerialExecutor* Self(void* ctx) { return static_cast<SerialExecutor*>(ctx); }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<executor::Task> tasks_;
  bool running_ = true;
  bool stop_ = false;
  std::atomic<bool> pretend_current_{false};
  std::thread worker_;
  ibmw_executor_base_t base_{
      .type = [](void*) { return ibmw::util::ToIbmwStringView("serial"); },
      .name = [](void*) { return ibmw::util::ToIbmwStringView("serial"); },
      .is_thread_safe = [](void*) { return true; },
      .is_in_current_executor =
          [](void* ctx) {
            return Self(ctx)->pretend_current_.load() || std::this_thread::get_id() == Self(ctx)->worker_.get_id();
          },
      .supports_timed_dispatch = [](void*) { return false; },
      .dispatch =
          [](void* ctx, ibmw_function_base_t* task) {
            auto* self = Self(ctx);
            {
              std::lock_guard lock(self->mutex_);
              self->tasks_.emplace_back(task);
            }
            self->cv_.notify_all();
          },
      .now = [](void*) -> uint64_t { return 0; },
      .dispatch_at_ns = nullptr,
      .ctx = this};
};

void UseExecutor(MsgHandleCatalog& catalog, SerialExecutor& executor) {
  catalog.RegisterGetExecutorFunc([&](std::string_view) { return executor.Handle(); });
  catalog.InitExecutor("serial");
}

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

// A handler on a serial executor shuts the catalog down while another
// message is queued behind it: the queued one cannot run first, so Shutdown
// must not wait for it; it is dropped instead.
TEST(MqttMsgHandleCatalog, ShutdownFromSerialExecutorWithQueuedMessageDoesNotDeadlock) {
  SerialExecutor executor;
  MsgHandleCatalog catalog;
  UseExecutor(catalog, executor);
  std::atomic<int> calls{0};
  std::promise<void> shutdown_returned;
  catalog.RegisterMsgHandle("t", [&](MQTTAsync_message*) {
    if (++calls == 1) {
      catalog.Shutdown();
      shutdown_returned.set_value();
    }
  });
  executor.Pause();
  Deliver(catalog, "t");
  Deliver(catalog, "t");
  ASSERT_EQ(executor.Queued(), 2u);
  executor.Run();
  ASSERT_EQ(shutdown_returned.get_future().wait_for(5s), std::future_status::ready);
  while (executor.Queued() != 0) std::this_thread::sleep_for(1ms);
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(calls.load(), 1);
}

// Shutdown from outside waits for queued tasks; they are dropped quickly.
TEST(MqttMsgHandleCatalog, ShutdownDrainsQueuedMessagesWithoutRunningHandlers) {
  SerialExecutor executor;
  MsgHandleCatalog catalog;
  UseExecutor(catalog, executor);
  std::atomic<int> calls{0};
  catalog.RegisterMsgHandle("t", [&](MQTTAsync_message*) { ++calls; });
  executor.Pause();
  Deliver(catalog, "t");
  auto shutdown = std::async(std::launch::async, [&] { catalog.Shutdown(); });
  EXPECT_EQ(shutdown.wait_for(100ms), std::future_status::timeout);
  executor.Run();
  EXPECT_EQ(shutdown.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(calls.load(), 0);
}

// A task released after the catalog is destroyed (drain skipped) only
// touches the separately allocated state (run under ASan).
TEST(MqttMsgHandleCatalog, QueuedMessageAfterCatalogDestroyedIsSafe) {
  SerialExecutor executor;
  std::atomic<int> calls{0};
  executor.Pause();
  {
    MsgHandleCatalog catalog;
    UseExecutor(catalog, executor);
    catalog.RegisterMsgHandle("t", [&](MQTTAsync_message*) { ++calls; });
    Deliver(catalog, "t");
    executor.PretendCurrent();
    catalog.Shutdown();  // drain skipped
  }
  executor.Run();
  while (executor.Queued() != 0) std::this_thread::sleep_for(1ms);
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(calls.load(), 0);
}

// The map is read without a lock once the client connects.
TEST(MqttMsgHandleCatalog, RegistrationAfterSealIsRejected) {
  MsgHandleCatalog catalog;
  catalog.RegisterMsgHandle("t", [](MQTTAsync_message*) {});
  catalog.Seal();
  EXPECT_ANY_THROW(catalog.RegisterMsgHandle("u", [](MQTTAsync_message*) {}));
  Deliver(catalog, "u");  // unknown topic: freed and ignored
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
