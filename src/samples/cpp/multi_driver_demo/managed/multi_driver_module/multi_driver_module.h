// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"

// IBMW_R1_EXEMPT_BEGIN
#include "dds_ext/dds_transport_driver.h"
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END

namespace ibmw::examples::cpp::multi_driver_demo::multi_driver_module {

// Listing two IBMW_DRIVER_ENTRY rows here forces the IBMW_USE_DRIVERS
// macro to instantiate one slot-match lambda per driver type within a
// single translation unit, which is the minimum compile-time witness
// that the macro composes correctly against driver headers from two
// independent extensions (DDS + Iceoryx).
class MultiDriverModule : public ibmw::Module {
 public:
  MultiDriverModule() = default;
  ~MultiDriverModule() override = default;

  ModuleDescriptor Info() const override {
    return ModuleDescriptor{.module_name = "MultiDriverModule"};
  }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

  auto GetLogger() { return core_.GetLogger(); }

 private:
  ibmw::Runtime core_;
  // IBMW_R2_EXEMPT_BEGIN
  // IBMW_R3_EXEMPT_BEGIN
  ibmw::extensions::dds_extension::DdsTransportDriver* dds_drv_ = nullptr;
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* iox_drv_ = nullptr;
  // IBMW_R3_EXEMPT_END
  // IBMW_R2_EXEMPT_END

  // IBMW_R2_EXEMPT_BEGIN
  IBMW_USE_DRIVERS(
      IBMW_DRIVER_ENTRY(ibmw::extensions::dds_extension::DdsTransportDriver,
                        dds_drv_),
      IBMW_DRIVER_ENTRY(ibmw::extensions::iceoryx_extension::IceoryxTransportDriver,
                        iox_drv_))
  // IBMW_R2_EXEMPT_END
};

}  // namespace ibmw::examples::cpp::multi_driver_demo::multi_driver_module
