#pragma once

#include <chrono>
#include <cerrno>
#include <cstring>
#include <cstdint>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

// Runtime discovery is a Linux-local, same-effective-UID protocol. V1 uses
// owner-only registry directories and sockets. Control RPCs are newline-framed
// JSON with an exact protocol_version and a bounded size/timeout. Stream frames
// use the fixed headers below followed by bounded JSON and payload bytes.
//
// Tap producers isolate the publish path with a bounded 1024-message queue,
// drop newest on overflow, and report the cumulative drop count. Playback is
// synchronous and applies socket backpressure; a timeout terminates the
// session with an error. Runtime IDs are exclusive while their PID/start-time
// registration is live, and discovery removes stale same-UID registrations.
inline constexpr uint32_t kRuntimeProtocolVersion = 1;
inline constexpr uint32_t kRuntimeTapMagic = 0x49424d57;
inline constexpr uint32_t kRuntimePlaybackMagic = 0x4942504c;
inline constexpr uint32_t kRuntimePlaybackTerminalMagic = 0x49425054;
inline constexpr size_t kRuntimeMaxControlMessage = 4 * 1024 * 1024;
inline constexpr size_t kRuntimeMaxStreamHeader = 1024 * 1024;
inline constexpr uint64_t kRuntimeMaxStreamPayload = 256ull * 1024ull * 1024ull;
inline constexpr auto kRuntimeSocketTimeout = std::chrono::seconds(5);

struct RuntimeStreamHeader {
  uint32_t magic;
  uint32_t protocol_version;
  uint32_t json_size;
  uint32_t reserved;
  uint64_t payload_size;
};
static_assert(sizeof(RuntimeStreamHeader) == 24);

struct RuntimeTerminalHeader {
  uint32_t magic;
  uint32_t protocol_version;
  uint32_t json_size;
  uint32_t reserved;
};
static_assert(sizeof(RuntimeTerminalHeader) == 16);

inline void ConfigureRuntimeSocket(int fd) {
  const timeval timeout{
      .tv_sec = static_cast<time_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              kRuntimeSocketTimeout).count()),
      .tv_usec = 0};
  MW_ASSERT(
      ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
          ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                       sizeof(timeout)) == 0,
      "Cannot configure Runtime IPC socket timeout: {}.",
      std::strerror(errno));
}

inline void RequireSameUid(int fd) {
#ifdef SO_PEERCRED
  struct ucred peer{};
  socklen_t length = sizeof(peer);
  MW_ASSERT(
      ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0,
      "Cannot inspect Runtime IPC peer credentials: {}.", std::strerror(errno));
  MW_ASSERT(
      peer.uid == ::geteuid(), "Runtime IPC peer UID {} differs from server UID {}.",
      static_cast<unsigned long>(peer.uid), static_cast<unsigned long>(::geteuid()));
#else
  static_cast<void>(fd);
#endif
}

}  // namespace ibmw::extensions::record_playback_extension
