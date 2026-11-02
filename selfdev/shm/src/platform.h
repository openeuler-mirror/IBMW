// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <deque>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ibmw_selfdev/shm/constants.h"
#include "ibmw_selfdev/shm/result.h"

// Thin Linux wrappers: memfd segments, shared futex, SEQPACKET sockets with
// fd passing. Nothing here knows about publishers or subscribers.
namespace ibmw::selfdev::shm::detail {

class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { Reset(); }
  UniqueFd(UniqueFd&& o) noexcept : fd_(o.Release()) {}
  UniqueFd& operator=(UniqueFd&& o) noexcept {
    if (this != &o) Reset(o.Release());
    return *this;
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  int Get() const noexcept { return fd_; }
  bool Valid() const noexcept { return fd_ >= 0; }
  int Release() noexcept {
    int f = fd_;
    fd_ = -1;
    return f;
  }
  void Reset(int fd = -1) noexcept;

 private:
  int fd_ = -1;
};

class Mapping {
 public:
  Mapping() noexcept = default;
  Mapping(void* p, size_t n) noexcept : ptr_(static_cast<uint8_t*>(p)), size_(n) {}
  ~Mapping() { Reset(); }
  Mapping(Mapping&& o) noexcept : ptr_(o.ptr_), size_(o.size_) {
    o.ptr_ = nullptr;
    o.size_ = 0;
  }
  Mapping& operator=(Mapping&& o) noexcept;
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  uint8_t* Data() const noexcept { return ptr_; }
  size_t Size() const noexcept { return size_; }
  bool Valid() const noexcept { return ptr_ != nullptr; }
  bool Contains(const void* p) const noexcept {
    auto* b = static_cast<const uint8_t*>(p);
    return b >= ptr_ && b < ptr_ + size_;
  }
  void Reset() noexcept;

 private:
  uint8_t* ptr_ = nullptr;
  size_t size_ = 0;
};

struct Segment {
  UniqueFd fd;
  Mapping map;
  bool read_only_enforced = false;  // F_SEAL_FUTURE_WRITE applied
};

// Creates a memfd of `size` bytes mapped read-write. When `seal_write` is
// true, F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_FUTURE_WRITE are applied after the
// mapping, so later mappings by peers can only be read-only.
Result<Segment> CreateSegment(const char* name, size_t size, bool seal_write, bool prefault);
// Maps a received segment; `writable` is false for data segments. The fd
// must be a regular file sealed with F_SEAL_SHRINK (kCorrupted otherwise).
Result<Mapping> MapSegment(int fd, size_t size, bool writable);

// Shared (not FUTEX_PRIVATE) futex; the word may live in a memfd mapping
// shared by several processes.
// Returns false on timeout. timeout_ns < 0 waits forever. Spurious returns
// are possible; callers must re-check their condition.
bool FutexWait(std::atomic<uint32_t>* word, uint32_t expected, int64_t timeout_ns) noexcept;
void FutexWake(std::atomic<uint32_t>* word, int count) noexcept;

// Sends one SEQPACKET message with up to 4 fds; non-blocking. On false,
// errno tells why (see SendWouldBlock()).
bool SendMessage(int sock, const void* data, size_t len, const int* fds, size_t nfds) noexcept;
// True for send errors that only mean "peer has not read yet" (its receive
// queue or our send buffer is full), as opposed to a broken connection.
bool SendWouldBlock(int err) noexcept;

// Control messages a peer could not take yet, sent in order once the socket
// is writable again (EPOLLOUT). Not thread-safe: the owner serialises access.
class SendQueue {
 public:
  enum class Status : uint8_t { kSent, kQueued, kFailed, kOverflow };
  // Bound per connection: a handshake announces up to every pool segment at
  // once, plus room for acks. A compliant peer drains its socket on its
  // control thread, so reaching it means the peer stopped reading. Memory
  // is only used while messages wait (about 64 bytes each).
  static constexpr size_t kMaxMessages = kMaxSegmentsPerPublisher + 64;
  static constexpr size_t kMaxBytes = 32;  // largest queued message (Welcome/SegmentAdd)

  // Sends now, or queues behind earlier messages. `fd` (-1: none) is passed
  // with the message and not owned; `owned` is passed instead when valid and
  // closed once the message is sent. kFailed: the connection is broken.
  Status Send(int sock, const void* data, size_t len, int fd, UniqueFd owned = UniqueFd()) noexcept;
  // Sends what the socket takes; false when the connection is broken.
  bool Flush(int sock) noexcept;
  bool Empty() const noexcept { return msgs_.empty(); }

 private:
  struct Msg {
    alignas(8) uint8_t data[kMaxBytes];
    uint32_t len;
    int fd;
    UniqueFd owned;
  };
  std::deque<Msg> msgs_;
};
// Receives one message. Returns bytes read, 0 on orderly shutdown, -1 with
// errno EAGAIN when empty, -1 otherwise. Received fds are CLOEXEC.
ssize_t ReceiveMessage(int sock, void* data, size_t cap, std::vector<UniqueFd>* fds) noexcept;

struct PeerCredentials {
  pid_t pid = 0;
  uid_t uid = 0;
  gid_t gid = 0;
};
bool GetPeerCredentials(int sock, PeerCredentials* out) noexcept;

// pidfd for `pid` (-1 when unavailable: old kernel, sandbox, no such pid;
// errno tells which). Readable once the process has exited.
int PidfdOpen(pid_t pid) noexcept;
// 1: the process is gone (ESRCH), 0: it exists or we cannot tell (EPERM).
// A pid reused by an unrelated process reads as alive: callers must treat
// "alive" as "not known dead".
int PidGone(pid_t pid) noexcept;

uint64_t Hash64(std::string_view s) noexcept;
uint64_t NowNs() noexcept;
uint64_t RandomU64() noexcept;
bool MakeDirectory(const std::string& path, mode_t mode, std::optional<gid_t> group) noexcept;

// Fault injection for tests; never set in production.
namespace fault {
extern std::atomic<uint32_t> send_would_block;  // next N SendMessage() calls fail with EAGAIN
extern std::atomic<bool> no_future_write_seal;  // CreateSegment() behaves as if the seal were unsupported
extern std::atomic<bool> no_pidfd;              // PidfdOpen() fails with ENOSYS
}  // namespace fault

}  // namespace ibmw::selfdev::shm::detail
