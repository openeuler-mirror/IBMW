// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "cpp/transport/transport_handle.h"

namespace ibmw::examples::cpp::playback_demo::generated {

struct PlaybackMessage {
  uint64_t sequence = 0;
  uint64_t timestamp = 0;
  std::string frame_id;
};

class PlaybackSubscriber {
 public:
  using Callback = std::function<void(const PlaybackMessage&)>;

  bool Subscribe(ibmw::transport::ReceiverRef subscriber, Callback callback);
};

}  // namespace ibmw::examples::cpp::playback_demo::generated
