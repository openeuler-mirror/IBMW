// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/executor/executor_bus_base.h"
#include "cpp/executor/executor.h"
#include "cpp/util/abi_table.h"
#include "cpp/util/string.h"

namespace ibmw::executor {

class ExecutorBusRef {
 public:
  ExecutorBusRef() = default;
  explicit ExecutorBusRef(const ibmw_executor_bus_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~ExecutorBusRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_executor_bus_base_t* NativeHandle() const {
    return base_ptr_;
  }

  ExecutorHandle GetExecutor(std::string_view executor_name) const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    const auto* executor = base_ptr_->get_executor(
        base_ptr_->ctx, ibmw::util::ToIbmwStringView(executor_name));
    return ibmw::util::IsCompleteAbiTable(executor) ? ExecutorHandle(executor) : ExecutorHandle();
  }

 private:
  const ibmw_executor_bus_base_t* base_ptr_ = nullptr;
};

}  // namespace ibmw::executor
