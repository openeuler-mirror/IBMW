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
// Pacing is the same as iox_loaned_ctx (absolute schedule, batch_size); the
// shared measurement window / publisher status file of iox_loaned_ctx is not
// carried over, the matrix does not use it.

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "cpp/module_base.h"

#include "bench_options.h"
#include "bench_plain_payloads.h"
#include "shm_ext/shm_ctx_publisher.h"

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
};

}  // namespace ibmw::samples::transport_benchmark::shm_loaned_ctx
