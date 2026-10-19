// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <mutex>

#include "engine/transport/serialized_subscriber.h"

namespace ibmw::runtime::core::transport {

// The native transport may retain queued callbacks after unsubscribing. Stop
// drains the current callback and prevents those queued callbacks from entering
// the consumer. The lifecycle owner must not call Stop from the callback.
class SerializedCallback {
 public:
  explicit SerializedCallback(SerializedMessageCallback callback)
      : callback_(std::move(callback)) {}

  void Invoke(SerializedMessage message) {
    std::lock_guard lock(mutex_);
    if (callback_) callback_(std::move(message));
  }

  void Stop() {
    std::lock_guard lock(mutex_);
    callback_ = {};
  }

 private:
  std::mutex mutex_;
  SerializedMessageCallback callback_;
};

}  // namespace ibmw::runtime::core::transport
