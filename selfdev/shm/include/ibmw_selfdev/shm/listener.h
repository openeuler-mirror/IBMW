// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>

#include "ibmw_selfdev/shm/result.h"
#include "ibmw_selfdev/shm/subscriber.h"

namespace ibmw::selfdev::shm {

struct ListenerOptions {
  std::string thread_name = "ibmw_shm_lsn";
  uint32_t capacity = 256;  // max attached subscribers (iceoryx default 256)
};

// One thread that sleeps on a shared futex until any attached subscriber has
// new data, then calls its callback. Idle cost: zero wake-ups.
// The callback is invoked when new data arrived since the previous callback
// for that subscriber (edge-triggered like iceoryx DATA_RECEIVED).
class Listener {
 public:
  using Callback = void (*)(Subscriber* subscriber, void* context);

  explicit Listener(const ListenerOptions& options = {});
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;

  bool Valid() const noexcept;
  Error InitError() const noexcept;

  Result<void> Attach(Subscriber& subscriber, Callback callback, void* context) noexcept;
  // After Detach() returns, the callback for this subscriber is not running
  // and will not run again (unless called from inside that callback).
  void Detach(Subscriber& subscriber) noexcept;
  uint32_t Size() const noexcept;

 private:
  std::shared_ptr<detail::ListenerImpl> impl_;
  Error init_error_ = Error::kOk;
};

}  // namespace ibmw::selfdev::shm
