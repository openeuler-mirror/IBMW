// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "opentelemetry_ext/util.h"

#include "opentelemetry/context/propagation/text_map_propagator.h"

namespace ibmw::extensions::opentelemetry_extension {

template <typename ContetxRefType>
class ContextCarrier : public opentelemetry::context::propagation::TextMapCarrier {
 public:
  explicit ContextCarrier(ContetxRefType ctx_ref)
      : ctx_ref_(ctx_ref) {}
  ContextCarrier() = default;

  virtual std::string_view Get(std::string_view key) const noexcept override {
    std::string real_key = std::string(kCtxKeyPrefix) + std::string(key);
    return ctx_ref_.GetMetaValue(real_key);
  }

  virtual void Set(std::string_view key, std::string_view value) noexcept override {
    std::string real_key = std::string(kCtxKeyPrefix) + std::string(key);
    ctx_ref_.SetMetaValue(real_key, value);
  }

  ContetxRefType ctx_ref_;
};
}  // namespace ibmw::extensions::opentelemetry_extension
