// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/parameter/parameter_handle_base.h"
#include "cpp/util/function.h"
#include "cpp/util/string.h"
#include "base/exception.h"

namespace ibmw::parameter {

using ParameterValReleaseCallback = util::Function<ibmw_function_parameter_val_release_callback_ops_t>;

class ParamsRef {
 public:
  ParamsRef() = default;
  explicit ParamsRef(const ibmw_parameter_handle_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~ParamsRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_parameter_handle_base_t* NativeHandle() const {
    return base_ptr_;
  }

  std::string GetParameter(std::string_view key) const {
    MW_ASSERT(base_ptr_, "Reference is null.");
    auto view_holder = base_ptr_->get_parameter(base_ptr_->ctx, util::ToIbmwStringView(key));
    std::string result;
    if (view_holder.parameter_val.len)
      result = util::ToStdString(view_holder.parameter_val);
    // Existing empty values also own a holder; only missing keys have no callback.
    if (view_holder.release_callback)
      ParameterValReleaseCallback(view_holder.release_callback)();
    return result;
  }

  void SetParameter(std::string_view key, std::string_view val) {
    MW_ASSERT(base_ptr_, "Reference is null.");
    base_ptr_->set_parameter(base_ptr_->ctx, util::ToIbmwStringView(key), util::ToIbmwStringView(val));
  }

 private:
  const ibmw_parameter_handle_base_t* base_ptr_ = nullptr;
};

}  // namespace ibmw::parameter
