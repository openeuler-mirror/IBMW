// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "c/util/buffer_base.h"

namespace ibmw::util {

class SimpleBufferArrayAllocator {
 public:
  static bool Reserve(ibmw_buffer_array_t* buffer_array, size_t new_cap) {
    // C ABI contract: never shrink, and leave the array unchanged on failure.
    if (new_cap <= buffer_array->capacity) return true;
    if (new_cap > SIZE_MAX / sizeof(ibmw_buffer_t)) [[unlikely]]
      return false;

    ibmw_buffer_t* new_data;
    try {
      new_data = new ibmw_buffer_t[new_cap];
    } catch (...) {
      return false;
    }

    ibmw_buffer_t* cur_data = buffer_array->data;
    if (cur_data) {
      memcpy(new_data, cur_data, buffer_array->len * sizeof(ibmw_buffer_t));
      delete[] cur_data;
    }

    buffer_array->data = new_data;
    buffer_array->capacity = new_cap;
    return true;
  }

  static ibmw_buffer_t Allocate(ibmw_buffer_array_t* buffer_array, size_t size) {
    // Grow the array before allocating the payload so a failed grow cannot leak it.
    if (buffer_array->capacity <= buffer_array->len) [[unlikely]] {
      static constexpr size_t kInitCapacitySize = 2;
      if (buffer_array->capacity > SIZE_MAX / 2) [[unlikely]]
        return ibmw_buffer_t{nullptr, 0};
      size_t new_capacity = (buffer_array->capacity < kInitCapacitySize)
                                ? kInitCapacitySize
                                : (buffer_array->capacity << 1);
      if (!Reserve(buffer_array, new_capacity)) [[unlikely]]
        return ibmw_buffer_t{nullptr, 0};
    }

    void* data = std::malloc(size);
    if (data == nullptr) [[unlikely]]
      return ibmw_buffer_t{nullptr, 0};

    return (buffer_array->data[buffer_array->len++] = ibmw_buffer_t{data, size});
  }

  static void Release(ibmw_buffer_array_t* buffer_array) {
    for (size_t ii = 0; ii < buffer_array->len; ++ii) {
      std::free(buffer_array->data[ii].data);
      buffer_array->data[ii].data = nullptr;
      buffer_array->data[ii].len = 0;
    }

    if (buffer_array->data) {
      delete[] buffer_array->data;
      buffer_array->data = nullptr;
      buffer_array->len = 0;
      buffer_array->capacity = 0;
    }
  }

  static const ibmw_buffer_array_allocator_t* NativeHandle() {
    static constexpr ibmw_buffer_array_allocator_t kSimpleBufferArrayAllocator{
        .reserve = [](void* ctx, ibmw_buffer_array_t* buffer_array, size_t new_cap) -> bool {
          return Reserve(buffer_array, new_cap);  //
        },
        .allocate = [](void* ctx, ibmw_buffer_array_t* buffer_array, size_t size) -> ibmw_buffer_t {
          return Allocate(buffer_array, size);
        },
        .release = [](void* ctx, ibmw_buffer_array_t* buffer_array) {
          Release(buffer_array);  //
        },
        .ctx = nullptr};

    return &kSimpleBufferArrayAllocator;
  }
};

class FlatBufferArrayAllocator {
 public:
  FlatBufferArrayAllocator(void* ptr, size_t size)
      : end_ptr_(static_cast<char*>(ptr) + size), cur_ptr_(ptr), base_(GenBase(this)) {}
  ~FlatBufferArrayAllocator() = default;

  FlatBufferArrayAllocator(const FlatBufferArrayAllocator&) = delete;
  FlatBufferArrayAllocator& operator=(const FlatBufferArrayAllocator&) = delete;

  const ibmw_buffer_array_allocator_t* NativeHandle() const { return &base_; }

  static bool Reserve(ibmw_buffer_array_t* buffer_array, size_t new_cap) {
    // C ABI contract: never shrink, and leave the array unchanged on failure
    // (a too large new_cap throws bad_array_new_length, caught below).
    if (new_cap <= buffer_array->capacity) return true;

    ibmw_buffer_t* cur_data = buffer_array->data;
    try {
      buffer_array->data = new ibmw_buffer_t[new_cap];
    } catch (...) {
      return false;
    }
    buffer_array->capacity = new_cap;

    if (cur_data) {
      memcpy(buffer_array->data, cur_data, buffer_array->len * sizeof(ibmw_buffer_t));
      delete[] cur_data;
    }
    return true;
  }

  ibmw_buffer_t Allocate(ibmw_buffer_array_t* buffer_array, size_t size) {
    if (out_of_memory_) [[unlikely]]
      return ibmw_buffer_t{nullptr, 0};

    // Compare with the remaining bytes: advancing first forms an out-of-range
    // pointer and wraps around for a huge size.
    if (cur_ptr_ == nullptr ||
        size > static_cast<size_t>(static_cast<char*>(end_ptr_) - static_cast<char*>(cur_ptr_)))
        [[unlikely]] {
      out_of_memory_ = true;
      return ibmw_buffer_t{nullptr, 0};
    }

    // Grow the array before taking the bytes so a failed grow consumes nothing.
    if (buffer_array->capacity <= buffer_array->len) [[unlikely]] {
      static constexpr size_t kInitCapacitySize = 2;
      if (buffer_array->capacity > SIZE_MAX / 2) [[unlikely]]
        return ibmw_buffer_t{nullptr, 0};
      size_t new_capacity = (buffer_array->capacity < kInitCapacitySize)
                                ? kInitCapacitySize
                                : (buffer_array->capacity << 1);
      if (!Reserve(buffer_array, new_capacity)) [[unlikely]]
        return ibmw_buffer_t{nullptr, 0};
    }

    // No alignment padding: buffers stay contiguous because users send the
    // region as one packet (see the iceoryx transport driver).
    void* data = cur_ptr_;
    cur_ptr_ = static_cast<char*>(cur_ptr_) + size;
    return (buffer_array->data[buffer_array->len++] = ibmw_buffer_t{data, size});
  }

  static void Release(ibmw_buffer_array_t* buffer_array) {
    for (size_t ii = 0; ii < buffer_array->len; ++ii) {
      buffer_array->data[ii].data = nullptr;
      buffer_array->data[ii].len = 0;
    }

    if (buffer_array->data) {
      delete[] buffer_array->data;
      buffer_array->data = nullptr;
      buffer_array->len = 0;
      buffer_array->capacity = 0;
    }
  }

  bool IsOutOfMemory() const { return out_of_memory_; }

  static ibmw_buffer_array_allocator_t GenBase(void* ctx) {
    return ibmw_buffer_array_allocator_t{
        .reserve = [](void* ctx, ibmw_buffer_array_t* buffer_array, size_t new_cap) -> bool {
          return Reserve(buffer_array, new_cap);  //
        },
        .allocate = [](void* ctx, ibmw_buffer_array_t* buffer_array, size_t size) -> ibmw_buffer_t {
          return static_cast<FlatBufferArrayAllocator*>(ctx)->Allocate(buffer_array, size);
        },
        .release = [](void* ctx, ibmw_buffer_array_t* buffer_array) {
          Release(buffer_array);  //
        },
        .ctx = ctx};
  }

 private:
  void* end_ptr_;
  void* cur_ptr_;

  bool out_of_memory_ = false;

  const ibmw_buffer_array_allocator_t base_;
};

}  // namespace ibmw::util
