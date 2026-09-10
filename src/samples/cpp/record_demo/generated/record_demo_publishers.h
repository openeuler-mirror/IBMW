// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>

#include "cpp/transport/transport_handle.h"

namespace ibmw::examples::cpp::record_demo::generated {

class CdrPublisher {
 public:
  bool Initialize(ibmw::transport::SenderRef publisher);
  void Publish(uint64_t sequence);

 private:
  ibmw::transport::SenderRef publisher_;
};

class Ros2Publisher {
 public:
  bool Initialize(ibmw::transport::SenderRef publisher);
  void Publish(uint64_t sequence);

 private:
  ibmw::transport::SenderRef publisher_;
};

}  // namespace ibmw::examples::cpp::record_demo::generated
