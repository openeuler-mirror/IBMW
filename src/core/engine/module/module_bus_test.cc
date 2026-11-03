// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include "cpp/module_base.h"
#include "engine/module/module_bus.h"

namespace ibmw::runtime::core::module {
namespace {

// CORE-024: a directly registered module's ctx is never cast to ibmw::Module.

struct CModule {
  ibmw_driver_slot_t slot = 0;
  void* driver = nullptr;
};

ibmw_module_base_t CModuleBase(CModule* module, bool with_callback) {
  ibmw_module_base_t base{};
  if (with_callback) {
    base.set_driver_slot = [](void* ctx, ibmw_driver_slot_t slot_id, void* driver_ptr) {
      auto* m = static_cast<CModule*>(ctx);
      m->slot = slot_id;
      m->driver = driver_ptr;
    };
  }
  base.ctx = module;
  return base;
}

class SlotModule : public ibmw::Module {
 public:
  ibmw::ModuleDescriptor Info() const override { return {}; }
  bool Setup(ibmw::Runtime) override { return true; }
  bool Run() override { return true; }
  void Stop() override {}
  void SetDriverSlot(ibmw_driver_slot_t slot_id, void* driver_ptr) override {
    slot = slot_id;
    driver = driver_ptr;
  }

  ibmw_driver_slot_t slot = 0;
  void* driver = nullptr;
};

TEST(ModuleBusDriverSlot, ReachesEachModuleThroughItsOwnCallback) {
  int driver = 0;
  CModule with_callback;
  // Holds bytes that would be read as a vtable if ctx were cast to ibmw::Module.
  CModule without_callback{0x5a5a5a5a5a5a5a5aULL, reinterpret_cast<void*>(0x5a5a5a5a5a5a5a5aULL)};
  const ibmw_module_base_t c_base = CModuleBase(&with_callback, true);
  const ibmw_module_base_t no_callback_base = CModuleBase(&without_callback, false);
  SlotModule cpp_module;

  ModuleBus bus;
  bus.RegisterModule(&c_base);
  bus.RegisterModule(&no_callback_base);
  bus.RegisterModule(cpp_module.NativeHandle());
  bus.BroadcastDriverSlot(42, &driver);

  EXPECT_EQ(with_callback.slot, 42u);
  EXPECT_EQ(with_callback.driver, &driver);
  EXPECT_EQ(without_callback.slot, 0x5a5a5a5a5a5a5a5aULL);
  EXPECT_EQ(cpp_module.slot, 42u);
  EXPECT_EQ(cpp_module.driver, &driver);
}

}  // namespace
}  // namespace ibmw::runtime::core::module
