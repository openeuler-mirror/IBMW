// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// API-045: a call suspended inside a filter survives RegisterFilter() and
// Clear() on the same bus.

#include <gtest/gtest.h>

#include <atomic>
#include <coroutine>
#include <optional>
#include <string>
#include <thread>

#include "cpp/co/sync_wait.h"
#include "cpp/service/rpc_co_filter.h"

namespace ibmw::rpc {
namespace {

// Suspends the awaiting coroutine until the test resumes it.
struct Gate {
  std::atomic<void*> waiting{nullptr};

  auto Wait() {
    struct Awaiter {
      Gate* gate;
      bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<> handle) noexcept {
        gate->waiting.store(handle.address(), std::memory_order_release);
      }
      void await_resume() const noexcept {}
    };
    return Awaiter{this};
  }

  void ResumeWhenSuspended() {
    void* address = nullptr;
    while ((address = waiting.load(std::memory_order_acquire)) == nullptr) std::this_thread::yield();
    std::coroutine_handle<>::from_address(address).resume();
  }
};

template <typename Mutate>
std::optional<Status> CallWhileMutating(Mutate mutate) {
  CoFilterBus bus;
  Gate gate;
  std::atomic<int> handled{0};
  // The coroutine reads its captures through the filter object after resuming.
  const std::string tag(64, 'f');
  std::string seen;
  bus.RegisterFilter([&gate, &handled, &seen, tag](ContextRef ctx, const void* req, void* rsp,
                                                   const CoRpcHandle& next) -> co::Task<Status> {
    co_await gate.Wait();
    seen = tag;
    ++handled;
    co_return co_await next(ctx, req, rsp);
  });

  std::optional<Status> status;
  std::thread caller([&] {
    auto handle = std::make_shared<CoRpcHandle>(
        [](ContextRef, const void*, void*) -> co::Task<Status> { co_return Status(9); });
    status = co::SyncWait(bus.InvokeRpc(std::move(handle), ContextRef(), nullptr, nullptr));
  });
  std::thread resumer([&] {
    while (gate.waiting.load(std::memory_order_acquire) == nullptr) std::this_thread::yield();
    mutate(bus);
    gate.ResumeWhenSuspended();
  });
  resumer.join();
  caller.join();
  EXPECT_EQ(handled.load(), 1);
  EXPECT_EQ(seen, tag);
  return status;
}

TEST(CoFilterBus, SuspendedCallSurvivesRegisterFilter) {
  auto status = CallWhileMutating([](CoFilterBus& bus) {
    bus.RegisterFilter([](ContextRef ctx, const void* req, void* rsp, const CoRpcHandle& next) -> co::Task<Status> {
      co_return co_await next(ctx, req, rsp);
    });
  });
  ASSERT_TRUE(status);
  EXPECT_EQ(status->Code(), 9u);
}

TEST(CoFilterBus, SuspendedCallSurvivesClear) {
  auto status = CallWhileMutating([](CoFilterBus& bus) { bus.Clear(); });
  ASSERT_TRUE(status);
  EXPECT_EQ(status->Code(), 9u);
}

TEST(CoFilterBus, FiltersRunInRegistrationOrderAndClearRemovesThem) {
  CoFilterBus bus;
  std::string trace;
  for (char name : {'a', 'b'}) {
    bus.RegisterFilter([&trace, name](ContextRef ctx, const void* req, void* rsp,
                                      const CoRpcHandle& next) -> co::Task<Status> {
      trace += name;
      co_return co_await next(ctx, req, rsp);
    });
  }
  auto call = [&] {
    auto handle = std::make_shared<CoRpcHandle>([&trace](ContextRef, const void*, void*) -> co::Task<Status> {
      trace += 'h';
      co_return Status(3);
    });
    return co::SyncWait(bus.InvokeRpc(std::move(handle), ContextRef(), nullptr, nullptr));
  };
  EXPECT_EQ(call()->Code(), 3u);
  EXPECT_EQ(trace, "bah");

  bus.Clear();
  trace.clear();
  EXPECT_EQ(call()->Code(), 3u);
  EXPECT_EQ(trace, "h");
}

}  // namespace
}  // namespace ibmw::rpc
