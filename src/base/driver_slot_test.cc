// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Unit tests for ibmw/src/api/c/driver_slot.h (typed-slot pkg ABI).
//   - DriverSlotFromString: compile-time stable, deterministic, distinct
//   - IBMW_DRIVER_ID macro: resolves slot id of a driver-like type
//   - Known well-known stable string ids: no collisions

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "c/driver_slot.h"

namespace ibmw {
namespace {

// Stub driver-like types used to validate IBMW_DRIVER_ID(...).
struct StubDdsDriver {
  static constexpr const char* kStableDriverId = "ibmw.transport.dds";
};
struct StubIceoryxDriver {
  static constexpr const char* kStableDriverId = "ibmw.transport.iceoryx";
};
struct StubRos2Driver {
  static constexpr const char* kStableDriverId = "ibmw.transport.ros2";
};
struct StubMqttDriver {
  static constexpr const char* kStableDriverId = "ibmw.transport.mqtt";
};
struct StubNetDriver {
  static constexpr const char* kStableDriverId = "ibmw.transport.net";
};
struct StubZenohDriver {
  static constexpr const char* kStableDriverId = "ibmw.transport.zenoh";
};

TEST(DriverSlotFromString, NonZeroForNonEmpty) {
  EXPECT_NE(DriverSlotFromString("ibmw.transport.dds"),
            IBMW_DRIVER_SLOT_INVALID);
  EXPECT_NE(DriverSlotFromString("a"), IBMW_DRIVER_SLOT_INVALID);
}

TEST(DriverSlotFromString, EmptyStringYieldsOffsetBasis) {
  // FNV-1a empty input == offset_basis = 0xcbf29ce484222325.
  // Not equal to IBMW_DRIVER_SLOT_INVALID (= 0).
  constexpr ibmw_driver_slot_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
  EXPECT_EQ(DriverSlotFromString(""), kFnvOffsetBasis);
}

TEST(DriverSlotFromString, IsConstexpr) {
  // Forces compile-time evaluation; if not constexpr-evaluable this
  // would fail to compile (verifies cross-.so determinism guarantee).
  constexpr ibmw_driver_slot_t kDds =
      DriverSlotFromString("ibmw.transport.dds");
  static_assert(kDds != 0, "FNV-1a of non-empty should be non-zero");
  EXPECT_EQ(kDds, DriverSlotFromString("ibmw.transport.dds"));
}

TEST(DriverSlotFromString, DistinctStringsDistinctSlots) {
  // Verify no collision among 6 known transport ids + a few neighbours.
  std::set<ibmw_driver_slot_t> slots;
  const char* kIds[] = {
      "ibmw.transport.dds",     "ibmw.transport.iceoryx",
      "ibmw.transport.ros2",    "ibmw.transport.mqtt",
      "ibmw.transport.net",     "ibmw.transport.zenoh",
      "ibmw.transport.dds.",    // trailing dot variant
      "ibmw.transport.DDS",     // case variant
      "ibmw.transport.iceory",  // truncation variant
      "",                       // empty
  };
  for (const char* s : kIds) {
    slots.insert(DriverSlotFromString(s));
  }
  EXPECT_EQ(slots.size(), sizeof(kIds) / sizeof(kIds[0]));
}

TEST(DriverSlotFromString, SameInputSameOutput) {
  // Determinism (across calls in same TU; cross-TU determinism is enforced
  // by FNV-1a being purely arithmetic on integer constants).
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(DriverSlotFromString("ibmw.transport.dds"),
              DriverSlotFromString("ibmw.transport.dds"));
  }
}

TEST(IbmwDriverIdMacro, ResolvesAtCompileTime) {
  constexpr ibmw_driver_slot_t kDds = IBMW_DRIVER_ID(StubDdsDriver);
  constexpr ibmw_driver_slot_t kIox = IBMW_DRIVER_ID(StubIceoryxDriver);
  static_assert(kDds != kIox, "stub driver ids must differ");
  EXPECT_NE(kDds, kIox);
  EXPECT_EQ(kDds, DriverSlotFromString("ibmw.transport.dds"));
  EXPECT_EQ(kIox, DriverSlotFromString("ibmw.transport.iceoryx"));
}

TEST(IbmwDriverIdMacro, AllKnownStableIdsNoCollision) {
  std::set<ibmw_driver_slot_t> slots = {
      IBMW_DRIVER_ID(StubDdsDriver),     IBMW_DRIVER_ID(StubIceoryxDriver),
      IBMW_DRIVER_ID(StubRos2Driver),    IBMW_DRIVER_ID(StubMqttDriver),
      IBMW_DRIVER_ID(StubNetDriver),     IBMW_DRIVER_ID(StubZenohDriver),
  };
  EXPECT_EQ(slots.size(), 6u) << "FNV-1a 64-bit collision among 6 known "
                                  "transport driver stable ids";
  EXPECT_EQ(slots.count(IBMW_DRIVER_SLOT_INVALID), 0u);
}

struct MultiDriverModuleStub {
  StubDdsDriver* dds_drv = nullptr;
  StubIceoryxDriver* iox_drv = nullptr;

  void Dispatch(ibmw_driver_slot_t slot_id, void* driver_ptr) {
    bool results[] = {
        ([&]() -> bool {
          if (slot_id == IBMW_DRIVER_ID(StubDdsDriver)) {
            dds_drv = static_cast<StubDdsDriver*>(driver_ptr);
            return true;
          }
          return false;
        }()),
        ([&]() -> bool {
          if (slot_id == IBMW_DRIVER_ID(StubIceoryxDriver)) {
            iox_drv = static_cast<StubIceoryxDriver*>(driver_ptr);
            return true;
          }
          return false;
        }()),
    };
    (void)results;
  }
};

TEST(MultiDriverDispatch, FirstSlotMatches) {
  MultiDriverModuleStub mod;
  StubDdsDriver dds;
  mod.Dispatch(IBMW_DRIVER_ID(StubDdsDriver), &dds);
  EXPECT_EQ(mod.dds_drv, &dds);
  EXPECT_EQ(mod.iox_drv, nullptr);
}

TEST(MultiDriverDispatch, SecondSlotMatches) {
  MultiDriverModuleStub mod;
  StubIceoryxDriver iox;
  mod.Dispatch(IBMW_DRIVER_ID(StubIceoryxDriver), &iox);
  EXPECT_EQ(mod.iox_drv, &iox);
  EXPECT_EQ(mod.dds_drv, nullptr);
}

TEST(MultiDriverDispatch, NoMatchLeavesPointersUnchanged) {
  MultiDriverModuleStub mod;
  StubMqttDriver unused;
  mod.Dispatch(IBMW_DRIVER_ID(StubMqttDriver), &unused);
  EXPECT_EQ(mod.dds_drv, nullptr);
  EXPECT_EQ(mod.iox_drv, nullptr);
}

TEST(MultiDriverDispatch, BothSlotsAcrossTwoCallsIndependent) {
  MultiDriverModuleStub mod;
  StubDdsDriver dds;
  StubIceoryxDriver iox;
  mod.Dispatch(IBMW_DRIVER_ID(StubDdsDriver), &dds);
  mod.Dispatch(IBMW_DRIVER_ID(StubIceoryxDriver), &iox);
  EXPECT_EQ(mod.dds_drv, &dds);
  EXPECT_EQ(mod.iox_drv, &iox);
}

}  // namespace
}  // namespace ibmw
