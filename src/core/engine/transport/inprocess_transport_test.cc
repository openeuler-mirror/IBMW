// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the local (in-process) transport:
// COMM-006: Shutdown must drain queued async deliveries before the catalog
//           that owns the handlers is released.
// COMM-007: cross-package delivery by direct copy requires a shared type
//           support implementation.
// COMM-037: one package failing does not drop the others, and inline
//           delivery leaves the serialization cache to the next driver.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "cpp/executor/executor.h"
#include "cpp/util/string.h"
#include "engine/transport/transport_driver_tools.h"
#include "engine/transport/inprocess_transport.h"
#include "engine/transport/transport_catalog.h"
#include "yaml-cpp/yaml.h"

namespace ibmw::runtime::core::transport {
namespace {

using namespace std::chrono_literals;

// Executor whose single worker runs tasks only after Release().
class GatedExecutor {
 public:
  GatedExecutor() {
    worker_ = std::thread([this] {
      std::unique_lock lock(mutex_);
      while (true) {
        cv_.wait(lock, [this] { return stop_ || (released_ && !tasks_.empty()); });
        if (tasks_.empty()) return;
        auto task = std::move(tasks_.front());
        tasks_.pop_front();
        lock.unlock();
        task();
        lock.lock();
      }
    });
  }
  ~GatedExecutor() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
      released_ = true;
    }
    cv_.notify_all();
    worker_.join();
  }
  void Release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }
  size_t Queued() {
    std::lock_guard lock(mutex_);
    return tasks_.size();
  }
  // Destroys queued tasks without running them (an executor shutting down).
  void DiscardAll() {
    std::deque<executor::Task> dropped;
    {
      std::lock_guard lock(mutex_);
      dropped.swap(tasks_);
    }
  }
  // Makes the transport believe it is shut down from this executor.
  void PretendCurrent() { pretend_current_ = true; }
  executor::ExecutorHandle Handle() { return executor::ExecutorHandle(&base_); }

 private:
  static GatedExecutor* Self(void* ctx) { return static_cast<GatedExecutor*>(ctx); }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<executor::Task> tasks_;
  bool released_ = false;
  bool stop_ = false;
  std::atomic<bool> pretend_current_{false};
  std::thread worker_;
  ibmw_executor_base_t base_{
      .type = [](void*) { return ibmw::util::ToIbmwStringView("gated"); },
      .name = [](void*) { return ibmw::util::ToIbmwStringView("gated"); },
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

// Copy-only type support for an int (no serialization), with distinct copy
// functions per instance kind so the transport can tell them apart.
template <int kTag>
const ibmw_type_support_base_t* IntTypeSupport() {
  static const ibmw_type_support_base_t ts{
      .type_name = [](void*) { return ibmw::util::ToIbmwStringView("test:Int"); },
      .create = [](void*) -> void* { return new int(0); },
      .destroy = [](void*, void* msg) { delete static_cast<int*>(msg); },
      .copy = [](void*, const void* from, void* to) {
        *static_cast<int*>(to) = *static_cast<const int*>(from) + kTag;
      },
      .move = [](void*, void* from, void* to) { *static_cast<int*>(to) = *static_cast<int*>(from); },
      .serialize = nullptr,
      .deserialize = nullptr,
      .serialization_types_supported_num = [](void*) -> size_t { return 0; },
      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return nullptr; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &ts;
}

TransportInfo Info(std::string pkg, std::string module, const ibmw_type_support_base_t* ts) {
  return {.msg_type = "test:Int",
          .topic_name = "/int",
          .pkg_path = std::move(pkg),
          .module_name = std::move(module),
          .msg_type_support_ref = ibmw::util::TypeSupportRef(ts)};
}

struct Fixture {
  TransportCatalog catalog;
  InProcessTransport transport;
  std::atomic<int> calls{0};
  std::atomic<int> last_value{-1};

  void Subscribe(TransportInfo info) {
    auto handler = std::make_unique<MessageHandlerWrapper>();
    handler->info = std::move(info);
    handler->callback = [this](MessageFrame& frame, std::function<void()>&&,
                               const internal::CallbackInvocationEnvelopeV1*) {
      if (frame.msg_ptr != nullptr) last_value = *static_cast<const int*>(frame.msg_ptr);
      ++calls;
    };
    ASSERT_TRUE(transport.OnMessage(*handler));
    ASSERT_TRUE(catalog.OnMessage(std::move(handler)));
  }

  void Publish(const TransportInfo& pub_info, int value) {
    ibmw::transport::Context context;
    MessageFrame frame{.info = pub_info, .msg_ptr = &value, .ctx_ref = ibmw::transport::ContextRef(context)};
    transport.Send(frame);
  }
};

TEST(InProcessTransportTest, ShutdownDrainsQueuedAsyncDeliveries) {
  GatedExecutor executor;
  Fixture f;
  f.transport.RegisterGetExecutorFunc([&](std::string_view) { return executor.Handle(); });
  f.transport.SetTransportCatalog(&f.catalog);
  f.transport.Initialize(YAML::Load("{subscriber_use_inline_executor: false, subscriber_executor: gated}"));
  f.Subscribe(Info("pkg", "sub", IntTypeSupport<0>()));
  f.transport.Start();

  const auto pub_info = Info("pkg", "pub", IntTypeSupport<0>());
  f.Publish(pub_info, 7);
  ASSERT_EQ(executor.Queued(), 1u);

  auto shutdown = std::async(std::launch::async, [&] { f.transport.Shutdown(); });
  // The queued delivery still references a catalog-owned handler, so
  // Shutdown must not return before it ran.
  EXPECT_EQ(shutdown.wait_for(200ms), std::future_status::timeout);
  EXPECT_EQ(f.calls.load(), 0);
  executor.Release();
  EXPECT_EQ(shutdown.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(f.calls.load(), 1);
  EXPECT_EQ(f.last_value.load(), 7);
}

// A delivery the executor discards after an incomplete drain (here: shutdown
// from the subscriber executor) releases its token after the transport is
// destroyed; the counter must outlive the transport (run under ASan).
TEST(InProcessTransportTest, DiscardedDeliveryAfterTransportDestroyedIsSafe) {
  GatedExecutor executor;
  {
    auto f = std::make_unique<Fixture>();
    f->transport.RegisterGetExecutorFunc([&](std::string_view) { return executor.Handle(); });
    f->transport.SetTransportCatalog(&f->catalog);
    f->transport.Initialize(YAML::Load("{subscriber_use_inline_executor: false, subscriber_executor: gated}"));
    f->Subscribe(Info("pkg", "sub", IntTypeSupport<0>()));
    f->transport.Start();
    f->Publish(Info("pkg", "pub", IntTypeSupport<0>()), 7);
    ASSERT_EQ(executor.Queued(), 1u);

    executor.PretendCurrent();
    f->transport.Shutdown();  // drain skipped
  }
  executor.DiscardAll();
  SUCCEED();
}

TEST(InProcessTransportTest, PublishWithoutSubscribersTakesNoToken) {
  GatedExecutor executor;
  Fixture f;
  f.transport.RegisterGetExecutorFunc([&](std::string_view) { return executor.Handle(); });
  f.transport.SetTransportCatalog(&f.catalog);
  f.transport.Initialize(YAML::Load("{subscriber_use_inline_executor: false, subscriber_executor: gated}"));
  f.Subscribe(Info("pkg", "sub", IntTypeSupport<0>()));
  f.transport.Start();
  auto other = Info("pkg", "pub", IntTypeSupport<0>());
  other.topic_name = "/nobody";
  f.Publish(other, 1);
  EXPECT_EQ(executor.Queued(), 0u);
  // Nothing in flight: Shutdown returns without waiting.
  auto shutdown = std::async(std::launch::async, [&] { f.transport.Shutdown(); });
  EXPECT_EQ(shutdown.wait_for(1s), std::future_status::ready);
}

TEST(InProcessTransportTest, CrossPackageCopyRequiresSharedTypeSupport) {
  Fixture f;
  f.transport.SetTransportCatalog(&f.catalog);
  f.transport.Initialize(YAML::Node());
  f.Subscribe(Info("sub_pkg", "sub", IntTypeSupport<100>()));
  f.transport.Start();

  // Different copy implementation in another package: must not be copied.
  f.Publish(Info("pub_pkg", "pub", IntTypeSupport<0>()), 7);
  f.Publish(Info("pub_pkg", "pub", IntTypeSupport<0>()), 8);
  EXPECT_EQ(f.calls.load(), 0);
  f.transport.Shutdown();
}

TEST(InProcessTransportTest, CrossPackageCopyWithSameTypeSupportIsDelivered) {
  Fixture f;
  f.transport.SetTransportCatalog(&f.catalog);
  f.transport.Initialize(YAML::Node());
  f.Subscribe(Info("sub_pkg", "sub", IntTypeSupport<100>()));
  f.transport.Start();

  f.Publish(Info("pub_pkg", "pub", IntTypeSupport<100>()), 7);
  EXPECT_EQ(f.calls.load(), 1);
  EXPECT_EQ(f.last_value.load(), 107);
  f.transport.Shutdown();
}

// Serializable int; serialize fails while `fail_next_serialize` is set.
std::atomic<int> serialize_calls{0};
std::atomic<bool> fail_next_serialize{false};
const ibmw_type_support_base_t* SerializableIntTypeSupport() {
  static const ibmw_string_view_t kTypes[] = {ibmw::util::ToIbmwStringView("test")};
  static const ibmw_type_support_base_t ts{
      .type_name = [](void*) { return ibmw::util::ToIbmwStringView("test:Int"); },
      .create = [](void*) -> void* { return new int(0); },
      .destroy = [](void*, void* msg) { delete static_cast<int*>(msg); },
      .copy = [](void*, const void* from, void* to) { *static_cast<int*>(to) = *static_cast<const int*>(from); },
      .move = [](void*, void* from, void* to) { *static_cast<int*>(to) = *static_cast<int*>(from); },
      .serialize = [](void*, ibmw_string_view_t, const void*, const ibmw_buffer_array_allocator_t*,
                      ibmw_buffer_array_t*) {
        ++serialize_calls;
        return !fail_next_serialize.exchange(false);
      },
      .deserialize = [](void*, ibmw_string_view_t, ibmw_buffer_array_view_t, void*) { return true; },
      .serialization_types_supported_num = [](void*) -> size_t { return 1; },
      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return kTypes; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &ts;
}

TEST(InProcessTransportTest, FailingPackageDoesNotDropTheOthers) {
  Fixture f;
  f.transport.SetTransportCatalog(&f.catalog);
  f.transport.Initialize(YAML::Node());
  f.Subscribe(Info("pub_pkg", "same", SerializableIntTypeSupport()));
  f.Subscribe(Info("other_a", "sub", SerializableIntTypeSupport()));
  f.Subscribe(Info("other_b", "sub", SerializableIntTypeSupport()));
  f.transport.Start();

  // Whichever cross-package subscriber comes first fails to serialize; the
  // retry for the second one succeeds.
  fail_next_serialize = true;
  f.Publish(Info("pub_pkg", "pub", SerializableIntTypeSupport()), 7);
  EXPECT_EQ(f.calls.load(), 2);
  f.transport.Shutdown();
}

TEST(InProcessTransportTest, InlineDeliveryKeepsSerializationCacheForNextDriver) {
  Fixture f;
  f.transport.SetTransportCatalog(&f.catalog);
  f.transport.Initialize(YAML::Node());
  f.Subscribe(Info("other_a", "sub", SerializableIntTypeSupport()));
  f.Subscribe(Info("other_b", "sub", SerializableIntTypeSupport()));
  f.transport.Start();

  serialize_calls = 0;
  const auto pub_info = Info("pub_pkg", "pub", SerializableIntTypeSupport());
  int value = 7;
  ibmw::transport::Context context;
  MessageFrame frame{.info = pub_info, .msg_ptr = &value, .ctx_ref = ibmw::transport::ContextRef(context)};
  f.transport.Send(frame);
  EXPECT_EQ(f.calls.load(), 2);
  // A driver after this one finds the bytes in the cache.
  EXPECT_NE(SerializeMsgWithCache(frame, "test"), nullptr);
  EXPECT_EQ(serialize_calls.load(), 1);
  f.transport.Shutdown();
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
