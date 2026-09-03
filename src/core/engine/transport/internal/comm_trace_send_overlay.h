// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "engine/transport/message_frame.h"
#include "engine/transport/transport_export.h"

namespace ibmw::runtime::core::transport::internal {

inline constexpr uint32_t kPublishEnvelopeAbiV1 = 1;
inline constexpr uint32_t kPublishEnvelopeValid = 1U << 0;
inline constexpr uint32_t kPublishPathSendAccess = 1;
inline constexpr uint64_t kCommTraceSendOverlayIdV1 =
    0x49424d5754524331ULL;  // "IBMWTRC1"

struct PublishEnvelopeV1 final {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t path;
  uint64_t publish_enter_ns;
  uint64_t reserved;

  constexpr bool valid() const noexcept {
    return abi_version == kPublishEnvelopeAbiV1 &&
           struct_size == sizeof(PublishEnvelopeV1) &&
           (flags & kPublishEnvelopeValid) != 0 &&
           (flags & ~kPublishEnvelopeValid) == 0 &&
           path == kPublishPathSendAccess && publish_enter_ns != 0 &&
           reserved == 0;
  }
};
static_assert(sizeof(PublishEnvelopeV1) == 32);
static_assert(alignof(PublishEnvelopeV1) == alignof(uint64_t));
static_assert(offsetof(PublishEnvelopeV1, abi_version) == 0);
static_assert(offsetof(PublishEnvelopeV1, struct_size) == 4);
static_assert(offsetof(PublishEnvelopeV1, flags) == 8);
static_assert(offsetof(PublishEnvelopeV1, path) == 12);
static_assert(offsetof(PublishEnvelopeV1, publish_enter_ns) == 16);
static_assert(offsetof(PublishEnvelopeV1, reserved) == 24);
static_assert(std::is_standard_layout_v<PublishEnvelopeV1>);
static_assert(std::is_trivial_v<PublishEnvelopeV1>);

struct OverlayDescriptorV1 final {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t envelope_size;
  uint32_t flags;
  uint64_t interface_id;

  constexpr bool valid() const noexcept {
    return abi_version == kPublishEnvelopeAbiV1 &&
           struct_size == sizeof(OverlayDescriptorV1) &&
           envelope_size == sizeof(PublishEnvelopeV1) && flags == 0 &&
           interface_id == kCommTraceSendOverlayIdV1;
  }
};
static_assert(sizeof(OverlayDescriptorV1) == 24);
static_assert(alignof(OverlayDescriptorV1) == alignof(uint64_t));
static_assert(std::is_standard_layout_v<OverlayDescriptorV1>);
static_assert(std::is_trivial_v<OverlayDescriptorV1>);

class IBMW_TRANSPORT_EXPORT CommTraceSendOverlayV1 {
 public:
  virtual ~CommTraceSendOverlayV1() = default;
  virtual const OverlayDescriptorV1& Descriptor() const noexcept = 0;
  virtual void SendObserved(MessageFrame&,
                            const PublishEnvelopeV1&) noexcept = 0;
};

inline constexpr PublishEnvelopeV1 InvalidPublishEnvelopeV1() noexcept {
  return {};
}

}  // namespace ibmw::runtime::core::transport::internal
