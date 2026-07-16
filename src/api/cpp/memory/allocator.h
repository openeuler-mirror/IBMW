// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/memory/allocator_base.h"
#include "base/exception.h"

namespace ibmw::allocator {

class AllocatorRef {
 public:
  AllocatorRef() = default;
  explicit AllocatorRef(const ibmw_allocator_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~AllocatorRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_allocator_base_t* NativeHandle() const { return base_ptr_; }

  void* GetThreadLocalBuf(size_t buf_size) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    return base_ptr_->get_thread_local_buf(base_ptr_->ctx, buf_size);
  }

 private:
  const ibmw_allocator_base_t* base_ptr_ = nullptr;
};

}  // namespace ibmw::allocator
