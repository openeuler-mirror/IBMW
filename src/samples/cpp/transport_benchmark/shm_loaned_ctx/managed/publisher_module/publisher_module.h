// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// shm_loaned_ctx publisher module: iox_loaned_ctx on the self-developed shm
// transport. Every tick builds a typed Plain bench message and hands it to
// ShmCtxMetaTypedPublisher<T> (shm_ext/shm_ctx_publisher.h), which loans a
// shm chunk, CDR-serializes the message directly into it behind the ctx-meta
// header and publishes -- no heap buffer, no memcpy on the publish side.
//
// The receive side is the standard OnMessage<T> stack (iox_send's subscriber
// module), so this lane and shm_send differ only in the publish path.
// Pacing is the same as iox_loaned_ctx (absolute schedule, batch_size). The
// shared measurement window and publisher status file of iox_loaned_ctx are
// active only when bench_options sets measure_begin_monotonic_ns /
// publisher_status_path (the shm core_metrics gate); the matrix sets neither.

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "cpp/module_base.h"

#include "bench_options.h"
#include "bench_plain_payloads.h"
// IBMW_R1_EXEMPT_BEGIN
#include "shm_ext/shm_ctx_publisher.h"
// IBMW_R1_EXEMPT_END

namespace ibmw::samples::transport_benchmark::shm_loaned_ctx {

class PublisherModule : public ibmw::Module {
 public:
  PublisherModule() = default;
  ~PublisherModule() override = default;

  ibmw::ModuleDescriptor Info() const override {
    return ibmw::ModuleDescriptor{.module_name = "ShmLoanedCtxPublisherModule"};
  }

  // Called by main() after engine.Initialize(); the base pointer is enough,
  // the ctx-meta publisher only uses TransportDriver virtuals.
  void SetDriver(ibmw::runtime::core::transport::TransportDriver* d) { driver_ = d; }

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();

  template <typename PlainT>
  struct TypedSlot {
    ibmw::transport::SenderRef sender;
    std::unique_ptr<ibmw::extensions::shm_extension::ShmCtxMetaTypedPublisher<PlainT>> typed_pub;
  };

  ibmw::Runtime core_;
  ibmw::runtime::core::transport::TransportDriver* driver_{nullptr};

#define IBMW_SHM_CTX_SLOT_DECL(LBL, N, ALIAS) TypedSlot<ALIAS> slot_##N##_;
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_SHM_CTX_SLOT_DECL)
#undef IBMW_SHM_CTX_SLOT_DECL

  BenchOptions opts_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  uint64_t measured_attempts_{0}, measured_success_{0}, measured_fail_{0};
  uint32_t first_attempt_seq_{0}, last_attempt_seq_{0};
  uint32_t first_success_seq_{0}, last_success_seq_{0};
  bool have_attempt_{false}, have_success_{false};
  bool completed_send_window_{false};
};

}  // namespace ibmw::samples::transport_benchmark::shm_loaned_ctx
