// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <unordered_map>
#include "iceoryx_posh/popo/listener.hpp"
#include "iceoryx_posh/popo/untyped_publisher.hpp"
#include "iceoryx_posh/popo/untyped_subscriber.hpp"
#include "base/string_util.h"

namespace ibmw::extensions::iceoryx_extension {

class IoxLoanedShm {
  friend class IoxPublisher;

 public:
  ~IoxLoanedShm() {
    if (release_func_) release_func_(*this);
  }

  IoxLoanedShm(const IoxLoanedShm&) = delete;
  IoxLoanedShm& operator=(const IoxLoanedShm&) = delete;

  void* Ptr() const { return ptr_; }
  size_t Size() const { return size_; }

 private:
  IoxLoanedShm(void* ptr, size_t size, std::function<void(IoxLoanedShm&)> release_func)
      : ptr_(ptr), size_(size), release_func_(release_func) {}

 private:
  void* ptr_;
  size_t size_;
  std::function<void(IoxLoanedShm&)> release_func_;
};

class IoxPublisher {
 public:
  IoxPublisher(std::string_view url, size_t shm_size);
  ~IoxPublisher() = default;

  IoxPublisher(const IoxPublisher&) = delete;
  IoxPublisher& operator=(const IoxPublisher&) = delete;

  IoxLoanedShm LoanShm(size_t min_size);
  void UpdateLoanShm(IoxLoanedShm& loaned_shm, size_t min_size);
  void SendShm(IoxLoanedShm& loaned_shm);

  // ----- Zero-copy POD-only path -----
  // These bypass IoxLoanedShm RAII (caller owns the lifecycle via the raw
  // chunk pointer), so they integrate with the core LoanedSampleHandle API.
  // LoanRaw returns nullptr on failure. The returned pointer is valid until
  // PublishRaw or ReleaseRaw is called.
  void* LoanRaw(size_t min_size);
  void PublishRaw(void* chunk_ptr) noexcept;
  void ReleaseRaw(void* chunk_ptr) noexcept;

 private:
  std::mutex mtx_;
  size_t shm_size_;
  iox::popo::UntypedPublisher publisher_;
};

class IceoryxBus {
 public:
  using MsgHandleFunc = std::function<void(iox::popo::UntypedSubscriber* subscriber)>;

 public:
  IceoryxBus() = default;
  ~IceoryxBus() = default;

  IceoryxBus(const IceoryxBus&) = delete;
  IceoryxBus& operator=(const IceoryxBus&) = delete;

  void Initialize(uint64_t shm_init_size, std::string_view runtime_id);
  void Shutdown();

  void RegisterPublisher(std::string_view url);
  void RegisterSubscriber(std::string_view url, MsgHandleFunc&& handle);

  IoxPublisher* GetPublisher(std::string_view url);

 private:
  uint64_t shm_init_size_;

  std::unique_ptr<iox::popo::Listener> iox_listener_ptr_;
  std::vector<std::unique_ptr<MsgHandleFunc>> msg_handle_vec_;

  std::unordered_map<
      std::string, std::unique_ptr<IoxPublisher>,
      ibmw::common::util::StringHash, std::equal_to<>>
      iox_pub_catalog_;
  std::unordered_map<
      std::string, std::unique_ptr<iox::popo::UntypedSubscriber>>
      iox_sub_catalog_;
};

}  // namespace ibmw::extensions::iceoryx_extension
