// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "engine/transport/message_frame.h"
#include "engine/service/service_message_frame.h"
#include "zenoh.h"

namespace ibmw::extensions::zenoh_extension {

// A loaned shm buffer. It is dropped unless it is published, also when a
// loan turned out too small or the send failed; the next collection of the
// pool reclaims it.
class ShmLoan {
 public:
  ShmLoan() { z_internal_null(&buf_); }
  ~ShmLoan() { Reset(); }
  ShmLoan(const ShmLoan&) = delete;
  ShmLoan& operator=(const ShmLoan&) = delete;

  // Loans `size` bytes, nullptr if the pool cannot. Only when the pool is
  // short, released loans are collected (alloc_gc_defrag collects only when
  // the pool is out of memory, not when it is fragmented) and it is compacted.
  unsigned char* Alloc(const z_loaned_shm_provider_t* provider, size_t size, z_alloc_alignment_t alignment) {
    Reset();
    z_buf_layout_alloc_result_t result;
    z_shm_provider_alloc(&result, provider, size, alignment);
    if (result.status != ZC_BUF_LAYOUT_ALLOC_STATUS_OK) {
      z_shm_provider_garbage_collect(provider);
      z_shm_provider_alloc_gc_defrag(&result, provider, size, alignment);
    }
    if (result.status != ZC_BUF_LAYOUT_ALLOC_STATUS_OK) return nullptr;
    buf_ = result.buf;
    return z_shm_mut_data_mut(z_loan_mut(buf_));
  }

  // Hands the buffer over to `payload`.
  void MoveTo(z_owned_bytes_t* payload) { z_bytes_from_shm_mut(payload, z_move(buf_)); }

  void Reset() { z_drop(z_move(buf_)); }

 private:
  z_owned_shm_mut_t buf_;
};

// Shm loan size per key, learned from messages that did not fit. Sizes only
// grow and stay within the pool: a message the pool cannot hold is sent over
// the network, and its key keeps loaning the whole pool.
class ShmLoanSizes {
 public:
  void Set(const std::string& key, uint64_t size) {
    std::unique_lock lock(mutex_);
    sizes_[key] = size;
  }

  uint64_t Get(const std::string& key, uint64_t fallback) const {
    std::shared_lock lock(mutex_);
    auto itr = sizes_.find(key);
    return itr == sizes_.end() ? fallback : itr->second;
  }

  // `need` bytes are needed; false if the pool cannot hold them.
  bool Need(const std::string& key, uint64_t need, uint64_t pool_size) {
    if (need > pool_size) return false;
    Raise(key, need);
    return true;
  }

  // A loan of `size` was too small: double it, at most to the pool size;
  // false if it already was the whole pool.
  bool Grow(const std::string& key, uint64_t size, uint64_t pool_size) {
    if (size >= pool_size) return false;
    Raise(key, size > pool_size / 2 ? pool_size : std::max<uint64_t>(size * 2, 1));
    return true;
  }

 private:
  void Raise(const std::string& key, uint64_t size) {
    std::unique_lock lock(mutex_);
    auto& cur = sizes_[key];
    cur = std::max(cur, size);
  }

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, uint64_t> sizes_;
};

inline std::pair<std::shared_ptr<ibmw::util::BufferArrayView>, size_t> SerializeMsgSupportedZenoh(
    runtime::core::transport::MessageFrame& msg_wrapper, std::string_view serialization_type, ibmw::util::BufferArrayAllocatorRef allocator) {
  auto& serialization_cache = msg_wrapper.serialization_cache;
  const auto& info = msg_wrapper.info;

  auto finditr = serialization_cache.find(serialization_type);
  if (finditr != serialization_cache.end()) {
    return {finditr->second, finditr->second->BufferSize()};
  }
  auto buffer_array_ptr = std::make_unique<ibmw::util::BufferArray>(allocator);
  bool serialize_ret = info.msg_type_support_ref.Serialize(
      serialization_type,
      msg_wrapper.msg_ptr,
      buffer_array_ptr->AllocatorNativeHandle(),
      buffer_array_ptr->BufferArrayNativeHandle());
  MW_ASSERT(serialize_ret, "Serialize failed.");

  return {nullptr, buffer_array_ptr->BufferSize()};
}

inline std::pair<std::shared_ptr<ibmw::util::BufferArrayView>, size_t> SerializeReqSupportedZenoh(
    runtime::core::service::ServiceMessageFrame& invoke_wrapper, std::string_view serialization_type, ibmw::util::BufferArrayAllocatorRef allocator) {
  const auto& info = invoke_wrapper.info;
  auto& req_serialization_cache = invoke_wrapper.req_serialization_cache;

  auto find_itr = req_serialization_cache.find(serialization_type);
  if (find_itr != req_serialization_cache.end())
    return {find_itr->second, find_itr->second->BufferSize()};

  auto buffer_array_ptr = std::make_unique<ibmw::util::BufferArray>(allocator);
  bool serialize_ret = info.req_type_support_ref.Serialize(
      serialization_type,
      invoke_wrapper.req_ptr,
      buffer_array_ptr->AllocatorNativeHandle(),
      buffer_array_ptr->BufferArrayNativeHandle());

  MW_ASSERT(serialize_ret, "Serialize failed.");

  return {nullptr, buffer_array_ptr->BufferSize()};
}

inline std::pair<std::shared_ptr<ibmw::util::BufferArrayView>, size_t> SerializeRspSupportedZenoh(
    runtime::core::service::ServiceMessageFrame& invoke_wrapper, std::string_view serialization_type, ibmw::util::BufferArrayAllocatorRef allocator) {
  const auto& info = invoke_wrapper.info;
  auto& rsp_serialization_cache = invoke_wrapper.rsp_serialization_cache;

  auto find_itr = rsp_serialization_cache.find(serialization_type);
  if (find_itr != rsp_serialization_cache.end())
    return {find_itr->second, find_itr->second->BufferSize()};

  auto buffer_array_ptr = std::make_unique<ibmw::util::BufferArray>(allocator);
  bool serialize_ret = info.rsp_type_support_ref.Serialize(
      serialization_type,
      invoke_wrapper.rsp_ptr,
      buffer_array_ptr->AllocatorNativeHandle(),
      buffer_array_ptr->BufferArrayNativeHandle());

  MW_ASSERT(serialize_ret, "Serialize failed.");

  return {nullptr, buffer_array_ptr->BufferSize()};
}

// A frame's length prefix is a uint32: a longer frame cannot be sent.
inline bool FitsFrame(uint64_t frame_len) noexcept { return frame_len <= std::numeric_limits<uint32_t>::max(); }

// Copies the buffers of an already serialized message to `dst`, one after the
// other; `dst` must hold BufferSize() bytes.

inline void CopyBuffers(const ibmw::util::BufferArrayView& buffers, unsigned char* dst) {
  for (size_t ii = 0; ii < buffers.Size(); ++ii) {
    const auto& buffer = buffers.Data()[ii];
    if (buffer.len == 0) continue;  // memcpy from a null pointer is undefined even for 0 bytes
    std::memcpy(dst, buffer.data, buffer.len);
    dst += buffer.len;
  }
}

}  // namespace ibmw::extensions::zenoh_extension
