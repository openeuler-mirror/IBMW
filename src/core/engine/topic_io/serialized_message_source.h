// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>

#include "engine/transport/serialized_subscriber.h"

namespace ibmw::runtime::core::topic_io {

class SerializedMessageSource {
 public:
  SerializedMessageSource() = default;
  virtual ~SerializedMessageSource() = default;

  SerializedMessageSource(const SerializedMessageSource&) = delete;
  SerializedMessageSource& operator=(const SerializedMessageSource&) = delete;

  virtual void Start(
      ibmw::transport::SerializedMessageCallback callback) = 0;
  virtual void Stop() = 0;
  virtual std::string Description() const = 0;
};

}  // namespace ibmw::runtime::core::topic_io
