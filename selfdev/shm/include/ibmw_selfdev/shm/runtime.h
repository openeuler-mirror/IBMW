// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <sys/types.h>

#include <optional>
#include <string>

#include "ibmw_selfdev/shm/result.h"

namespace ibmw::selfdev::shm {

// Every setting is resolved the same way: the option if set, else its
// environment variable, else the default.
//   root_dir  $IBMW_SHM_ROOT    Linux: /dev/shm if writable, else /tmp;
//                               OHOS: none, Init fails with kInvalidArg
//   domain    $IBMW_SHM_DOMAIN  "default"
//   group     $IBMW_SHM_GROUP   none (numeric gid)
// The service directory is <root>/ibmw_shm.<euid or g<gid>>.<domain> and may
// be at most 63 bytes (socket paths must fit sun_path); a longer one fails
// with kInvalidArg and a message on stderr.
struct RuntimeOptions {
  std::string name;         // informational, e.g. "iceoryx<pid>"
  std::string domain;       // empty: $IBMW_SHM_DOMAIN, else "default"
  std::string root_dir;     // empty: $IBMW_SHM_ROOT, else the platform default
  // Processes of different users only meet through a shared group: every
  // one of them must set the same gid (directory 0770, sockets 0660, peers
  // of that gid accepted). Without it each user gets its own directory.
  std::optional<gid_t> group;
};

// Per-process runtime: owns the control thread (epoll, no timeout) and the
// listener notification segment. There is no daemon. Init() is optional;
// the first Publisher/Subscriber/Listener initialises it with defaults.
// Init() on a running runtime succeeds if it resolves to the same service
// directory (root, owner, domain) and fails with kAlreadyInitialized
// otherwise, so a conflicting domain is never silently ignored. When the
// implicit initialisation fails, Publisher/Subscriber/Listener report its
// error as their InitError().
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
