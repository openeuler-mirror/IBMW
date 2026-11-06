// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/module_base.h"

// IBMW_MULTI_DRIVER_DEMO_WITH_SHM selects the self-developed shm driver as
// the second driver (multi_driver_module_shm target) instead of Iceoryx.
// IBMW_R1_EXEMPT_BEGIN
#include "dds_ext/dds_transport_driver.h"
#if defined(IBMW_MULTI_DRIVER_DEMO_WITH_SHM)
#include "shm_ext/shm_transport_driver.h"
#else
#include "iceoryx_ext/iceoryx_transport_driver.h"
#endif
// IBMW_R1_EXEMPT_END

namespace ibmw::examples::cpp::multi_driver_demo::multi_driver_module {

// Listing two IBMW_DRIVER_ENTRY rows here forces the IBMW_USE_DRIVERS
// macro to instantiate one slot-match lambda per driver type within a
// single translation unit, which is the minimum compile-time witness
// that the macro composes correctly against driver headers from two
// independent extensions (DDS + Iceoryx, or DDS + shm).
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
#if defined(IBMW_MULTI_DRIVER_DEMO_WITH_SHM)
  ibmw::extensions::shm_extension::ShmTransportDriver* shm_drv_ = nullptr;
#else
  ibmw::extensions::iceoryx_extension::IceoryxTransportDriver* iox_drv_ = nullptr;
#endif
  // IBMW_R3_EXEMPT_END
  // IBMW_R2_EXEMPT_END

  // IBMW_R2_EXEMPT_BEGIN
#if defined(IBMW_MULTI_DRIVER_DEMO_WITH_SHM)
  IBMW_USE_DRIVERS(
      IBMW_DRIVER_ENTRY(ibmw::extensions::dds_extension::DdsTransportDriver,
                        dds_drv_),
      IBMW_DRIVER_ENTRY(ibmw::extensions::shm_extension::ShmTransportDriver,
                        shm_drv_))
#else
  IBMW_USE_DRIVERS(
      IBMW_DRIVER_ENTRY(ibmw::extensions::dds_extension::DdsTransportDriver,
                        dds_drv_),
      IBMW_DRIVER_ENTRY(ibmw::extensions::iceoryx_extension::IceoryxTransportDriver,
                        iox_drv_))
#endif
  // IBMW_R2_EXEMPT_END
};

}  // namespace ibmw::examples::cpp::multi_driver_demo::multi_driver_module
