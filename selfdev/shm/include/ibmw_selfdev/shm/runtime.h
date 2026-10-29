// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <sys/types.h>

#include <optional>
#include <string>

#include "ibmw_selfdev/shm/result.h"

namespace ibmw::selfdev::shm {

struct RuntimeOptions {
  std::string name;                // informational, e.g. "iceoryx<pid>"
  std::string domain = "default";  // overridden by $IBMW_SHM_DOMAIN
  std::string root_dir;            // empty: $IBMW_SHM_ROOT, else /tmp
  std::optional<gid_t> group;      // allow peers of this group (dir 0770)
};

// Per-process runtime: owns the control thread (epoll, no timeout) and the
// listener notification segment. There is no daemon. Init() is optional;
// the first Publisher/Subscriber/Listener initialises it with defaults.
class Runtime {
 public:
  static Result<void> Init(const RuntimeOptions& options);
  static bool IsInitialized() noexcept;
  // Directory holding the publisher sockets of the active domain.
  static std::string ServiceDirectory();
  // Stops the control thread. All Publishers/Subscribers/Listeners must be
  // destroyed first. Mainly for tests; a process may simply exit.
  static void Shutdown() noexcept;
};

}  // namespace ibmw::selfdev::shm
