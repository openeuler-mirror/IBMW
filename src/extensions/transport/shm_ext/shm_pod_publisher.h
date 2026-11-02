// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Raw zero-copy POD publisher (counterpart of iceoryx_ext's
// IoxRawPodPublisher / CreateRawPodPublisher): borrow N bytes of shm, write
// in place, publish. Wire format is kPodOnly under
// "/transport_zc_pod/<topic>/<msg_type>"; the receive side is
// ShmTransportDriver::RegisterPodSubscriber.
//
// Pre-condition: CreateShmRawPodPublisher runs while the driver is in its
// Init state (after Engine::Initialize, before Engine::Start), like the
// iceoryx factory.
//
// Lifetime: both classes keep a raw ShmTransportDriver*, so they must not
// outlive the driver. Return or publish every loan before engine shutdown;
// a loan still held makes the driver's shutdown preparation time out.
//
//   auto pub = CreateShmRawPodPublisher(driver, "topic", "BenchPod", 4096);
//   auto sample = pub->Borrow(4096);
//   if (sample.valid()) { Fill(sample.data(), sample.size()); pub->Publish(std::move(sample)); }

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "engine/transport/loaned_sample_handle.h"
#include "shm_ext/shm_transport_driver.h"

namespace ibmw::extensions::shm_extension {

// Move-only loan; returned to the driver unpublished on destruction.
class ShmLoanedBytes {
 public:
  ShmLoanedBytes() = default;
  ShmLoanedBytes(ShmTransportDriver* driver, runtime::core::transport::LoanedSampleHandle handle) noexcept
      : driver_(driver), handle_(handle) {}
  ~ShmLoanedBytes() { Release(); }

  ShmLoanedBytes(const ShmLoanedBytes&) = delete;
  ShmLoanedBytes& operator=(const ShmLoanedBytes&) = delete;
  ShmLoanedBytes(ShmLoanedBytes&& other) noexcept : driver_(other.driver_), handle_(other.handle_) {
    other.driver_ = nullptr;
    other.handle_ = {};
  }
  ShmLoanedBytes& operator=(ShmLoanedBytes&& other) noexcept {
    if (this != &other) {
      Release();
      driver_ = other.driver_;
      handle_ = other.handle_;
      other.driver_ = nullptr;
      other.handle_ = {};
    }
    return *this;
  }

  bool valid() const noexcept { return driver_ != nullptr && handle_.user_payload_ptr != nullptr; }
  // The driver that lent the chunk.
  ShmTransportDriver* Owner() const noexcept { return driver_; }
  void* data() noexcept { return handle_.user_payload_ptr; }
  size_t size() const noexcept { return handle_.user_payload_size; }

  // Hands the handle over for publishing; the sample becomes invalid.
  runtime::core::transport::LoanedSampleHandle Extract() noexcept {
    auto h = handle_;
    handle_ = {};
    driver_ = nullptr;
    return h;
  }

 private:
  void Release() noexcept {
    if (valid()) driver_->ReleaseLoaned(handle_);
    driver_ = nullptr;
    handle_ = {};
  }

  ShmTransportDriver* driver_ = nullptr;
  runtime::core::transport::LoanedSampleHandle handle_;
};

class ShmRawPodPublisher {
 public:
  ShmRawPodPublisher(ShmTransportDriver* driver, std::string topic, std::string msg_type) : driver_(driver) {
    info_.topic_name = std::move(topic);
    info_.msg_type = std::move(msg_type);
  }

  // Invalid sample if `bytes` is 0 or the loan fails.
  ShmLoanedBytes Borrow(size_t bytes) noexcept {
    if (bytes == 0) return {};
    runtime::core::transport::LoanedSampleHandle handle;
    if (!driver_->LoanSample(info_, bytes, runtime::core::transport::LoanMode::kPodOnly, nullptr, handle)) return {};
    return ShmLoanedBytes(driver_, handle);
  }

  // False if the sample is invalid or its chunk was not published (e.g. a
  // stale or duplicate loan). A sample of another driver is not published
  // here; the driver that lent it takes the chunk back.
  bool Publish(ShmLoanedBytes&& sample) noexcept {
    if (!sample.valid()) return false;
    ShmTransportDriver* owner = sample.Owner();
    auto handle = sample.Extract();
    if (driver_->TryPublishLoaned(handle)) return true;
    if (handle.driver_token != nullptr) owner->ReleaseLoaned(handle);
    return false;
  }

  const runtime::core::transport::TransportInfo& info() const noexcept { return info_; }

 private:
  ShmTransportDriver* driver_;
  runtime::core::transport::TransportInfo info_;
};

// Registers the POD topic; nullptr if `driver` is null or registration fails
// (wrong state, conflicting size hint).
inline std::unique_ptr<ShmRawPodPublisher> CreateShmRawPodPublisher(ShmTransportDriver* driver,
                                                                   std::string_view topic,
                                                                   std::string_view msg_type,
                                                                   size_t initial_size_hint) {
  if (driver == nullptr) return nullptr;
  if (!driver->EnsurePodTopic(topic, msg_type, initial_size_hint)) return nullptr;
  return std::make_unique<ShmRawPodPublisher>(driver, std::string(topic), std::string(msg_type));
}

}  // namespace ibmw::extensions::shm_extension
