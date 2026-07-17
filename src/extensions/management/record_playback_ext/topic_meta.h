// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "cpp/util/buffer.h"

namespace ibmw::extensions::record_playback_extension {

struct TopicMeta {
  uint64_t id = 0;
  std::string topic_name;
  std::string msg_type;
  std::string serialization_type;
  bool record_enabled = true;
  bool cache_last_msg = false;
  double sample_freq = 0;
};

}  // namespace ibmw::extensions::record_playback_extension
