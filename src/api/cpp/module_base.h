// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdio>
#include <optional>
#include <string_view>

#include "c/driver_slot.h"
#include "c/module_base.h"
#include "cpp/core.h"
#include "cpp/util/string.h"

namespace ibmw {

/**
 * @brief All modules need inherit this interface class
 *
 */
class Module {
 public:
  Module() : base_(GenBase(this)) {}
  virtual ~Module() = default;

  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  /**
   * @brief Get module info
   *
   * @return module info
   */
  virtual ModuleDescriptor Info() const = 0;

  /**
   * @brief Called when the module is loaded by the framework
   *
   * @param rt Runtime handle providing access to framework services
   * @return load result
   */
  virtual bool Setup(Runtime rt) = 0;

  /**
   * @brief Called when the framework starts running
   *
   * @return start result
   */
  virtual bool Run() = 0;

  /**
   * @brief Called when the framework stops running
   *
   */
  virtual void Stop() = 0;

  /**
   * @brief OPTIONAL hook called by the framework after Engine::Initialize
   *        and before Engine::Start, once per (this module, broadcast
   *        transport driver). Typed-slot dispatch lets a module receive
   *        a typed pointer to a transport driver implementation when it
   *        needs direct driver access for zero-copy loan APIs.
   *
   * Default implementation is a no-op. Modules opt in via the
   * `IBMW_USE_DRIVER(DriverT, member_ptr)` macro (single-driver) or
   * `IBMW_USE_DRIVERS(...)` (multi-driver), which generate an override
   * that compares the broadcast slot id against `DriverT::kStableDriverId`
   * (typed at compile time, no string comparison) and stores the typed
   * pointer into the supplied member.
   *
   * Direct user overrides of this hook are discouraged: cast of `void*`
   * to a transport-specific concrete type couples user code to a
   * transport plugin and is policed by the R7 red-line in
   * check_red_lines.sh. Prefer the typed transport-agnostic APIs
   * (`ibmw::transport::SenderAccess<T>`, `ReceiverAccess<T>`,
   * `LoanedSample<T>`, `LoanedSubscriber<T>`); use the macros above
   * only for legitimate direct-driver-access needs (e.g. perf benches),
   * which are audited as transport-coupled by design (R10).
   *
   * @param slot_id    Stable 64-bit driver slot id (FNV-1a of stable id).
   * @param driver_ptr Type-erased driver pointer; user macro casts to
   *                   the concrete DriverT*.
   */
  virtual void SetDriverSlot(ibmw_driver_slot_t slot_id, void* driver_ptr) {
    (void)slot_id;
    (void)driver_ptr;
  }

  /**
   * @brief Get native handle
   *
   * @return Native handle
   */
  const ibmw_module_base_t* NativeHandle() const { return &base_; }

 private:
  static ibmw_module_base_t GenBase(void* ctx) {
    return ibmw_module_base_t{
        .info = [](void* ctx) -> ibmw_module_info_t {
          try {
            auto module_info = static_cast<Module*>(ctx)->Info();
            return ibmw_module_info_t{
                .module_name = ibmw::util::ToIbmwStringView(module_info.module_name),
                .major_version = module_info.major_version,
                .minor_version = module_info.minor_version,
                .patch_version = module_info.patch_version,
                .build_version = module_info.build_version,
                .author = ibmw::util::ToIbmwStringView(module_info.author),
                .description = ibmw::util::ToIbmwStringView(module_info.description)};
          } catch (...) {
            return ibmw_module_info_t{};
          }
        },
        .setup = [](void* ctx, const ibmw_core_base_t* core_ptr) -> bool {
          auto* module_ptr = static_cast<Module*>(ctx);

          // An incompatible core fails this module's setup; there is no
          // logger to report through yet.
          std::optional<Runtime> runtime;
          try {
            runtime.emplace(core_ptr);
          } catch (const std::exception& e) {
            std::fprintf(stderr, "[IBMW ERROR] Module setup rejected the core: %s\n", e.what());
            return false;
          }
          module_ptr->core_ptr_ = core_ptr;

          try {
            return module_ptr->Setup(*runtime);
          } catch (const std::exception& e) {
            MW_LOG(
                Runtime(module_ptr->core_ptr_).GetLogger(), ibmw::common::util::kLogLevelError,
                "Setup module get exception: {}", e.what());
            return false;
          } catch (...) {
            MW_LOG(
                Runtime(module_ptr->core_ptr_).GetLogger(), ibmw::common::util::kLogLevelError,
                "Setup module get unknown exception.");
            return false;
          }
        },
        .run = [](void* ctx) -> bool {
          auto* module_ptr = static_cast<Module*>(ctx);

          try {
            return module_ptr->Run();
          } catch (const std::exception& e) {
            MW_LOG(
                Runtime(module_ptr->core_ptr_).GetLogger(), ibmw::common::util::kLogLevelError,
                "Run module get exception: {}", e.what());
            return false;
          } catch (...) {
            MW_LOG(
                Runtime(module_ptr->core_ptr_).GetLogger(), ibmw::common::util::kLogLevelError,
                "Run module get unknown exception.");
            return false;
          }
        },
        .stop = [](void* ctx) {
          auto* module_ptr = static_cast<Module*>(ctx);

          try {
            module_ptr->Stop();
          } catch (const std::exception& e) {
            MW_LOG(
                Runtime(module_ptr->core_ptr_).GetLogger(), ibmw::common::util::kLogLevelError,
                "Stop module get exception: {}", e.what());
          } catch (...) {
            MW_LOG(
                Runtime(module_ptr->core_ptr_).GetLogger(), ibmw::common::util::kLogLevelError,
                "Stop module get unknown exception.");
          }
        },
        .set_driver_slot = [](void* ctx, ibmw_driver_slot_t slot_id, void* driver_ptr) {
          try {
            static_cast<Module*>(ctx)->SetDriverSlot(slot_id, driver_ptr);
          } catch (...) {
          }
        },
        .ctx = ctx};
  }

 private:
  const ibmw_module_base_t base_;
  const ibmw_core_base_t* core_ptr_ = nullptr;
};

}  // namespace ibmw

// ---------------------------------------------------------------------------
// IBMW_USE_DRIVER — typed-slot driver opt-in (single-driver case).
//
// Generates an override of `ibmw::Module::SetDriverSlot` that compares the
// broadcast slot id against `DriverT::kStableDriverId` (typed at compile
// time, no string comparison) and, on match, stores a `DriverT*` into the
// supplied member pointer.
//
// Usage (inside `class MyModule : public ibmw::Module { ... };`):
//
//   class MyModule : public ibmw::Module {
//    public:
//     // ... Setup / Run / Stop / Info ...
//
//    private:
//     ibmw::runtime::core::transport::IceoryxTransportDriver* iox_drv_ = nullptr;
//     IBMW_USE_DRIVER(ibmw::runtime::core::transport::IceoryxTransportDriver,
//                     iox_drv_)
//   };
//
// Constraints:
//   - DriverT must publish a `static constexpr ibmw_driver_slot_t
//     kStableDriverId` constant (provided automatically by the
//     `IBMW_DECLARE_DRIVER_SLOT` macro on the driver header).
//   - Single-driver opt-in only. For modules needing more than one
//     transport driver, use `IBMW_USE_DRIVERS(...)` below.
// ---------------------------------------------------------------------------
#define IBMW_USE_DRIVER(DriverT, member_ptr)                                    \
 public:                                                                        \
  void SetDriverSlot(ibmw_driver_slot_t slot_id, void* driver_ptr) override {   \
    if (slot_id == IBMW_DRIVER_ID(DriverT)) {                                   \
      (member_ptr) = static_cast<DriverT*>(driver_ptr);                         \
    }                                                                           \
  }                                                                             \
 private:

// ---------------------------------------------------------------------------
// IBMW_USE_DRIVERS — typed-slot driver opt-in (multi-driver case).
//
// Variadic counterpart of IBMW_USE_DRIVER above. Generates an override of
// `ibmw::Module::SetDriverSlot` that dispatches the broadcast slot id to one
// of N driver entries, each declared via the IBMW_DRIVER_ENTRY(DriverT,
// member_ptr) helper. Each entry contributes a constexpr slot id check + a
// typed pointer assignment; on first match the dispatch returns. On no
// match the override is a no-op (matches the base-class default).
//
// Implementation notes:
//   - Uses C++17 fold expression over a parameter pack of zero-argument
//     lambdas, each wrapping one (DriverT, member) pair. The fold short
//     circuits on the first lambda that returns true (a slot match).
//   - Compile-time guarantee: each entry references DriverT::kStableDriverId
//     directly, so a typo in the driver name fails at compile time.
//   - All pointer casts go through static_cast<DriverT*>; no reinterpret.
//   - Inserted as `public: ... private:` to match the IBMW_USE_DRIVER
//     surface; class default access is restored after the macro.
//
// Constraints:
//   - Each DriverT must publish `static constexpr const char* kStableDriverId`
//     (provided automatically by IBMW_DECLARE_DRIVER_SLOT in the driver
//     header).
//   - Members must be assignable from DriverT* (typically T* member fields).
//   - At least one IBMW_DRIVER_ENTRY argument is required; the macro is not
//     valid with zero entries (use the base-class no-op default for that).
//
// Usage:
//
//   class MultiDriverModule : public ibmw::Module {
//    public:
//     // ... Setup / Run / Stop / Info ...
//
//    private:
//     ibmw::runtime::core::transport::DdsTransportDriver*    dds_drv_ = nullptr;
//     ibmw::runtime::core::transport::IceoryxTransportDriver* iox_drv_ = nullptr;
//     IBMW_USE_DRIVERS(
//         IBMW_DRIVER_ENTRY(ibmw::runtime::core::transport::DdsTransportDriver,
//                           dds_drv_),
//         IBMW_DRIVER_ENTRY(ibmw::runtime::core::transport::IceoryxTransportDriver,
//                           iox_drv_))
//   };
// ---------------------------------------------------------------------------

// Each IBMW_DRIVER_ENTRY expands to an immediately-invoked lambda
// expression (IIFE) that evaluates to a `bool`: `true` on slot match
// (after storing the typed pointer into the member) or `false` on miss.
// The lambda captures `slot_id` and `driver_ptr` from the enclosing
// SetDriverSlot override scope by reference (and `this` for the member).
//
// Because each entry is already a `bool` value (not a callable), the
// IBMW_USE_DRIVERS macro folds them via a braced-init array
// `bool results[] = { __VA_ARGS__ };`. This dodges the comma-operator
// pitfall that would otherwise reduce a `(entry1, entry2)()` form to a
// single invocation of `entry2()`.
#define IBMW_DRIVER_ENTRY(DriverT, member_ptr)                                 \
  ([&]() -> bool {                                                              \
    if (slot_id == IBMW_DRIVER_ID(DriverT)) {                                   \
      (member_ptr) = static_cast<DriverT*>(driver_ptr);                         \
      return true;                                                              \
    }                                                                           \
    return false;                                                               \
  }())

#define IBMW_USE_DRIVERS(...)                                                   \
 public:                                                                        \
  void SetDriverSlot(ibmw_driver_slot_t slot_id, void* driver_ptr) override {   \
    (void)slot_id;                                                              \
    (void)driver_ptr;                                                           \
    bool ibmw_use_drivers_results[] = {__VA_ARGS__};                            \
    (void)ibmw_use_drivers_results;                                             \
  }                                                                             \
 private:
