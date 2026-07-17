// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "c/util/buffer_base.h"

namespace ibmw::util {

class ZenohBufferArrayAllocator {
 public:
  ZenohBufferArrayAllocator(uint64_t z_shm_capacity, void* z_shm_head_ptr)
      : z_shm_capacity_(z_shm_capacity),
        z_shm_cur_ptr_(z_shm_head_ptr),
        base_(GenBase(this)) {}

  ~ZenohBufferArrayAllocator() = default;
  ZenohBufferArrayAllocator(const ZenohBufferArrayAllocator&) = delete;
  ZenohBufferArrayAllocator& operator=(const ZenohBufferArrayAllocator&) = delete;
  const ibmw_buffer_array_allocator_t* NativeHandle() const { return &base_; }

  bool Reserve(ibmw_buffer_array_t* buffer_array, size_t new_cap) {
    ibmw_buffer_t* cur_data = buffer_array->data;

    buffer_array->data = new ibmw_buffer_t[new_cap];
    buffer_array->capacity = new_cap;

    if (cur_data) {
      memcpy(buffer_array->data, cur_data, buffer_array->len * sizeof(ibmw_buffer_t));
      delete[] cur_data;
    }

    return true;
  }

  ibmw_buffer_t Allocate(ibmw_buffer_array_t* buffer_array, size_t size) {
    void* data = z_shm_cur_ptr_;
    z_shm_cur_ptr_ = static_cast<char*>(z_shm_cur_ptr_) + size;
    z_shm_used_size_ += size;

    if (z_shm_used_size_ > z_shm_capacity_) [[unlikely]] {
      z_is_shm_enough_ = false;
      return ibmw_buffer_t{nullptr, 0};
    }

    if (data == nullptr) [[unlikely]]
      return ibmw_buffer_t{nullptr, 0};

    if (buffer_array->capacity > buffer_array->len) {
      return (buffer_array->data[buffer_array->len++] = ibmw_buffer_t{data, size});
    }

    static constexpr size_t kInitCapacitySzie = 2;
    size_t new_capacity = (buffer_array->capacity < kInitCapacitySzie)
                              ? kInitCapacitySzie
                              : (buffer_array->capacity << 1);

    Reserve(buffer_array, new_capacity);

    return (buffer_array->data[buffer_array->len++] = ibmw_buffer_t{data, size});
  }

  void Release(ibmw_buffer_array_t* buffer_array) {
    if (buffer_array->data) delete[] buffer_array->data;
  }

  bool IsShmEnough() { return z_is_shm_enough_; }

  static const ibmw_buffer_array_allocator_t GenBase(void* ctx) {
    return ibmw_buffer_array_allocator_t{
        .reserve = [](void* ctx, ibmw_buffer_array_t* buffer_array, size_t new_cap) -> bool {
          return static_cast<ZenohBufferArrayAllocator*>(ctx)->Reserve(buffer_array, new_cap);  //
        },
        .allocate = [](void* ctx, ibmw_buffer_array_t* buffer_array, size_t size) -> ibmw_buffer_t {
          return static_cast<ZenohBufferArrayAllocator*>(ctx)->Allocate(buffer_array, size);
        },
        .release = [](void* ctx, ibmw_buffer_array_t* buffer_array) {
          static_cast<ZenohBufferArrayAllocator*>(ctx)->Release(buffer_array);  //
        },
        .ctx = ctx};
  }

 private:
  uint64_t z_shm_capacity_;
  uint64_t z_shm_used_size_ = 0;

  void* z_shm_cur_ptr_;

  bool z_is_shm_enough_ = true;

  const ibmw_buffer_array_allocator_t base_;
};

}  // namespace ibmw::util
