// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "cpp/parameter/parameter_handle.h"
#include "cpp/util/function.h"
#include "cpp/util/string.h"
#include "engine/module/parameter_handle.h"

namespace ibmw::runtime::core::parameter {

class ParameterHandleAccess {
 public:
  explicit ParameterHandleAccess(ParameterHandle& parameter_handle)
      : base_(GenBase(&parameter_handle)) {}
  ~ParameterHandleAccess() = default;

  ParameterHandleAccess(const ParameterHandleAccess&) = delete;
  ParameterHandleAccess& operator=(const ParameterHandleAccess&) = delete;

  const ibmw_parameter_handle_base_t* NativeHandle() const { return &base_; }

 private:
  static ibmw_parameter_handle_base_t GenBase(void* ctx) {
    return ibmw_parameter_handle_base_t{
        .struct_size = sizeof(ibmw_parameter_handle_base_t),
        .get_parameter = [](void* ctx, ibmw_string_view_t key) -> ibmw_parameter_val_view_holder_t {
          auto ptr = static_cast<ParameterHandle*>(ctx)->GetParameter(ibmw::util::ToStdStringView(key));
          if (!ptr) [[unlikely]] {
            return ibmw_parameter_val_view_holder_t{
                .parameter_val = ibmw_string_view_t{nullptr, 0},
                .release_callback = nullptr};
          }

          auto* f = new ibmw::parameter::ParameterValReleaseCallback();
          (*f) = [ptr, f]() { delete f; };

          return ibmw_parameter_val_view_holder_t{
              .parameter_val = ibmw::util::ToIbmwStringView(*ptr),
              .release_callback = f->NativeHandle()};
        },
        .set_parameter = [](void* ctx, ibmw_string_view_t key, ibmw_string_view_t val) {
          auto ptr = std::make_shared<std::string>(ibmw::util::ToStdString(val));
          static_cast<ParameterHandle*>(ctx)->SetParameter(ibmw::util::ToStdStringView(key), ptr);  //
        },
        .ctx = ctx};
  }

 private:
  const ibmw_parameter_handle_base_t base_;
};

}  // namespace ibmw::runtime::core::parameter
