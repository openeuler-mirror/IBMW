// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Module for the installed coro package test: a chain of co_awaited Tasks on
// an IBMW executor (TaskPromiseBase::Start / Finish live in
// libibmw_selfdev_coro, so this package records it as DT_NEEDED).
// With IBMW_CORO_FIXTURE_CRASH=overflow|null the module crashes in Run()
// instead, to check the crash report for package code through bin/ibmw.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>
#include <tuple>

#include "cpp/co/async_scope.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/inline_executor.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "cpp/co/task.h"
#include "cpp/module_base.h"
#include "pkg/pkg_macro.h"

namespace ibmw_installed_coro_package {

volatile int g_recurse_limit = 1 << 30;  // never reached
volatile int* volatile g_null = nullptr;

// Default visibility so the symbolized report part can name them.
extern "C" __attribute__((noinline, visibility("default"))) int CoroFixtureCrashRecurse(int depth) {
  volatile char pad[1024];
  pad[0] = static_cast<char>(depth);
  if (depth >= g_recurse_limit) return pad[0];
  return CoroFixtureCrashRecurse(depth + 1) + pad[0];
}

extern "C" __attribute__((noinline, visibility("default"))) void CoroFixtureCrashNullWrite() {
  *g_null = 42;
}

class CoroFixtureModule : public ibmw::Module {
 public:
  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "CoroFixtureModule"};
  }

  bool Setup(ibmw::Runtime core) override {
    core_ = core;
    ctx_ = ibmw::co::IbmwContext(core_.GetExecutor());
    return static_cast<bool>(core_.GetExecutor().GetExecutor("work_executor"));
  }

  bool Run() override {
    if (const char* crash = std::getenv("IBMW_CORO_FIXTURE_CRASH")) {
      if (std::strcmp(crash, "overflow") == 0) return CoroFixtureCrashRecurse(0) != 0;
      if (std::strcmp(crash, "null") == 0) CoroFixtureCrashNullWrite();
    }
    scope_.spawn(ibmw::co::On(ibmw::co::InlineExecutor(), Sum(1000)));
    return true;
  }

  void Stop() override { ibmw::co::SyncWait(scope_.complete()); }

 private:
  auto GetLogger() { return core_.GetLogger(); }

  static ibmw::co::Task<uint64_t> Add(uint64_t a, uint64_t b) { co_return a + b; }

  ibmw::co::Task<void> Sum(uint64_t n) {
    co_await ibmw::co::Schedule(ctx_.GetExecutor("work_executor"));
    uint64_t sum = 0;
    for (uint64_t ii = 1; ii <= n; ++ii) sum = co_await Add(sum, ii);
    MW_INFO("coro fixture result={}", sum);
    co_return;
  }

  ibmw::Runtime core_;
  ibmw::co::IbmwContext ctx_;
  ibmw::co::AsyncScope scope_;
};

}  // namespace ibmw_installed_coro_package

static std::tuple<std::string_view, std::function<ibmw::Module*()>> ibmw_module_register_array[]{
    {"CoroFixtureModule",
     []() -> ibmw::Module* { return new ibmw_installed_coro_package::CoroFixtureModule(); }}};
IBMW_PKG_MAIN(ibmw_module_register_array)
