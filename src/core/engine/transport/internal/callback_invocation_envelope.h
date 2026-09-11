// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ibmw::runtime::core::transport::internal {

inline constexpr uint32_t kCallbackInvocationEnvelopeAbiV1 = 1;
inline constexpr uint32_t kCallbackInvocationEnvelopeValid = 1U << 0;

using CallbackInvocationEnterV1 =
    void (*)(void* context, uint64_t callback_registration_id) noexcept;
using CallbackInvocationExitV1 =
    void (*)(void* context, uint32_t status) noexcept;

struct CallbackInvocationEnvelopeV1 final {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved;
  void* context;
  CallbackInvocationEnterV1 enter;
  CallbackInvocationExitV1 exit;

  constexpr bool valid() const noexcept {
    return abi_version == kCallbackInvocationEnvelopeAbiV1 &&
           struct_size == sizeof(CallbackInvocationEnvelopeV1) &&
           flags == kCallbackInvocationEnvelopeValid && reserved == 0 &&
           context != nullptr && enter != nullptr && exit != nullptr;
  }
};

static_assert(sizeof(void*) == 8);
static_assert(sizeof(CallbackInvocationEnterV1) == 8);
static_assert(sizeof(CallbackInvocationExitV1) == 8);
static_assert(sizeof(CallbackInvocationEnvelopeV1) == 40);
static_assert(alignof(CallbackInvocationEnvelopeV1) == alignof(uint64_t));
static_assert(offsetof(CallbackInvocationEnvelopeV1, abi_version) == 0);
static_assert(offsetof(CallbackInvocationEnvelopeV1, struct_size) == 4);
static_assert(offsetof(CallbackInvocationEnvelopeV1, flags) == 8);
static_assert(offsetof(CallbackInvocationEnvelopeV1, reserved) == 12);
static_assert(offsetof(CallbackInvocationEnvelopeV1, context) == 16);
static_assert(offsetof(CallbackInvocationEnvelopeV1, enter) == 24);
static_assert(offsetof(CallbackInvocationEnvelopeV1, exit) == 32);
static_assert(std::is_standard_layout_v<CallbackInvocationEnvelopeV1>);
static_assert(std::is_trivial_v<CallbackInvocationEnvelopeV1>);

}  // namespace ibmw::runtime::core::transport::internal
