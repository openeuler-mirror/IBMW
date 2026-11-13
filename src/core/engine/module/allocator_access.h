// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdlib>

#include "c/memory/allocator_base.h"

namespace ibmw::runtime::core::allocator {

class AllocatorAccess {
 public:
  explicit AllocatorAccess()
      : base_(GenBase(this)) {}
  ~AllocatorAccess() = default;

  AllocatorAccess(const AllocatorAccess&) = delete;
  AllocatorAccess& operator=(const AllocatorAccess&) = delete;

  const ibmw_allocator_base_t* NativeHandle() const { return &base_; }

 private:
  static void* GetThreadLocalBuf(size_t buf_size) {
    constexpr size_t kMaxThreadLocalBufSize = 1024 * 1024 * 16;
    constexpr size_t kMinThreadLocalBufSize = 1024 * 4;

    if (buf_size > kMaxThreadLocalBufSize) return nullptr;

    struct Buf {
      Buf() : buf(malloc(kMinThreadLocalBufSize)), size(kMinThreadLocalBufSize) {}
      ~Buf() {
        if (buf != nullptr) free(buf);
      }
      void* buf;
      size_t size;
    };

    thread_local Buf thread_local_buf;

    if (thread_local_buf.size >= buf_size)
      return thread_local_buf.buf;

    if (thread_local_buf.buf != nullptr)
      free(thread_local_buf.buf);

    while (thread_local_buf.size < buf_size)
      thread_local_buf.size <<= 1;

    thread_local_buf.buf = malloc(thread_local_buf.size);

    return thread_local_buf.buf;
  }

  static ibmw_allocator_base_t GenBase(void* ctx) {
    return ibmw_allocator_base_t{
        .struct_size = sizeof(ibmw_allocator_base_t),
        .get_thread_local_buf = [](void* ctx, size_t buf_size) -> void* {
          return AllocatorAccess::GetThreadLocalBuf(buf_size);
        },
        .ctx = ctx};
  }

 private:
  const ibmw_allocator_base_t base_;
};

}  // namespace ibmw::runtime::core::allocator
