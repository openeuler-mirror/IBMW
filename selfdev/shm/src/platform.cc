// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <random>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#endif
#ifndef F_GET_SEALS
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif
#ifndef F_SEAL_GROW
#define F_SEAL_GROW 0x0004
#endif
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif
#ifndef SO_PEERGROUPS
#define SO_PEERGROUPS 59  // Linux 4.13, asm-generic value (x86, arm, arm64)
#endif
#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434  // same number on every architecture
#endif

namespace ibmw::selfdev::shm::detail {

namespace fault {
std::atomic<uint32_t> send_would_block{0};
std::atomic<uint32_t> send_no_buffers{0};
std::atomic<bool> no_future_write_seal{false};
std::atomic<bool> no_pidfd{false};
std::atomic<bool> kill_eperm{false};
std::atomic<uint32_t> map_segment_fail{0};
std::atomic<uint32_t> grow_bad_alloc{0};
std::atomic<uint32_t> offer_bad_alloc{0};
std::atomic<size_t> send_queue_limit{0};
std::atomic<void (*)()> subscriber_shutdown_pause{nullptr};

bool Hit(std::atomic<uint32_t>& counter) noexcept {
  for (uint32_t n = counter.load(std::memory_order_relaxed); n != 0;) {
    if (counter.compare_exchange_weak(n, n - 1, std::memory_order_relaxed)) return true;
  }
  return false;
}
}  // namespace fault

void UniqueFd::Reset(int fd) noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = fd;
}

Mapping& Mapping::operator=(Mapping&& o) noexcept {
  if (this != &o) {
    Reset();
    ptr_ = o.ptr_;
    size_ = o.size_;
    o.ptr_ = nullptr;
    o.size_ = 0;
  }
  return *this;
}

void Mapping::Reset() noexcept {
  if (ptr_ != nullptr) ::munmap(ptr_, size_);
  ptr_ = nullptr;
  size_ = 0;
}

namespace {

int MemfdCreate(const char* name, unsigned flags) {
#ifdef SYS_memfd_create
  return static_cast<int>(::syscall(SYS_memfd_create, name, flags));
#else
  (void)name;
  (void)flags;
  errno = ENOSYS;
  return -1;
#endif
}

}  // namespace

Result<Segment> CreateSegment(const char* name, size_t size, bool seal_write, bool prefault) {
  Segment seg;
  seg.fd = UniqueFd(MemfdCreate(name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (!seg.fd.Valid()) return Error::kSystem;
  if (::ftruncate(seg.fd.Get(), static_cast<off_t>(size)) != 0) {
    return errno == ENOMEM || errno == ENOSPC ? Error::kNoMemory : Error::kSystem;
  }
  int flags = MAP_SHARED | (prefault ? MAP_POPULATE : 0);
  void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, flags, seg.fd.Get(), 0);
  if (p == MAP_FAILED) return errno == ENOMEM ? Error::kNoMemory : Error::kSystem;
  seg.map = Mapping(p, size);
  int seals = F_SEAL_SHRINK | F_SEAL_GROW;
  if (seal_write && !fault::no_future_write_seal.load(std::memory_order_relaxed) &&
      ::fcntl(seg.fd.Get(), F_ADD_SEALS, seals | F_SEAL_FUTURE_WRITE) == 0) {
    seg.read_only_enforced = true;
  } else if (::fcntl(seg.fd.Get(), F_ADD_SEALS, seals) != 0) {
    return Error::kSystem;
  }
  return seg;
}

Result<Mapping> MapSegment(int fd, size_t size, bool writable) {
  // Only shmem (memfd, tmpfs) is accepted: a hugetlbfs memfd or a file on a
  // disk filesystem behaves differently on access, truncation and
  // writeback. memfds report TMPFS_MAGIC on Linux and on OHOS (HongMeng or
  // Linux kernel, measured).
  constexpr unsigned long kTmpfsMagic = 0x01021994ul;  // linux/magic.h TMPFS_MAGIC
  if (fault::Hit(fault::map_segment_fail)) return Error::kNoMemory;
  struct statfs sfs {};
  if (::fstatfs(fd, &sfs) != 0) return Error::kSystem;
  if (static_cast<unsigned long>(sfs.f_type) != kTmpfsMagic) return Error::kCorrupted;
  struct stat st {};
  if (::fstat(fd, &st) != 0) return Error::kSystem;
  if (!S_ISREG(st.st_mode) || size == 0 || static_cast<uint64_t>(st.st_size) < size) return Error::kCorrupted;
  // A peer-provided file that can still shrink would turn our accesses into
  // SIGBUS; only accept memfds sealed against shrinking.
  int seals = ::fcntl(fd, F_GET_SEALS);
  if (seals < 0 || (seals & F_SEAL_SHRINK) == 0) return Error::kCorrupted;
  int prot = PROT_READ | (writable ? PROT_WRITE : 0);
  void* p = ::mmap(nullptr, size, prot, MAP_SHARED, fd, 0);
  if (p == MAP_FAILED) return errno == ENOMEM ? Error::kNoMemory : Error::kSystem;
  return Mapping(p, size);
}

bool FutexWait(std::atomic<uint32_t>* word, uint32_t expected, int64_t timeout_ns) noexcept {
  static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
  struct timespec ts {};
  struct timespec* tsp = nullptr;
  if (timeout_ns >= 0) {
    ts.tv_sec = static_cast<time_t>(timeout_ns / 1000000000);
    ts.tv_nsec = static_cast<long>(timeout_ns % 1000000000);
    tsp = &ts;
  }
  long r = ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAIT, expected, tsp,
                     nullptr, 0);
  return !(r != 0 && errno == ETIMEDOUT);
}

void FutexWake(std::atomic<uint32_t>* word, int count) noexcept {
  ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAKE, count, nullptr, nullptr, 0);
}

bool SendMessage(int sock, const void* data, size_t len, const int* fds, size_t nfds) noexcept {
  if (nfds > 4) {
    errno = EINVAL;
    return false;
  }
  if (fault::Hit(fault::send_would_block)) {
    errno = EAGAIN;
    return false;
  }
  if (fault::Hit(fault::send_no_buffers)) {
    errno = ENOBUFS;
    return false;
  }
  struct iovec iov {};
  iov.iov_base = const_cast<void*>(data);
  iov.iov_len = len;
  alignas(struct cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 4)];
  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  if (nfds > 0) {
    std::memset(ctrl, 0, sizeof(ctrl));
    msg.msg_control = ctrl;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
    struct cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
    std::memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
  }
  for (;;) {
    ssize_t n = ::sendmsg(sock, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n == static_cast<ssize_t>(len)) return true;
    if (n < 0 && errno == EINTR) continue;
    if (n >= 0) errno = EMSGSIZE;  // SEQPACKET never sends a partial record
    return false;
  }
}

bool SendWouldBlock(int err) noexcept {
  return err == EAGAIN || err == EWOULDBLOCK || err == ENOBUFS || err == ENOMEM;
}

SendQueue::Status SendQueue::Send(int sock, const void* data, size_t len, int fd, UniqueFd owned) noexcept {
  if (owned.Valid()) fd = owned.Get();
  if (msgs_.empty()) {
    if (SendMessage(sock, data, len, &fd, fd >= 0 ? 1 : 0)) return Status::kSent;
    if (!SendWouldBlock(errno)) return Status::kFailed;
  }
  const size_t limit = fault::send_queue_limit.load(std::memory_order_relaxed);
  if (msgs_.size() >= (limit != 0 ? limit : kMaxMessages)) return Status::kOverflow;
  if (len > kMaxBytes) return Status::kFailed;
  try {
    msgs_.emplace_back();
  } catch (...) {  // std::bad_alloc: cannot keep the order any more
    return Status::kFailed;
  }
  Msg& m = msgs_.back();
  std::memcpy(m.data, data, len);
  m.len = static_cast<uint32_t>(len);
  m.fd = fd;
  m.owned = std::move(owned);
  return Status::kQueued;
}

bool SendQueue::Flush(int sock) noexcept {
  no_buffers_ = false;
  while (!msgs_.empty()) {
    Msg& m = msgs_.front();
    if (!SendMessage(sock, m.data, m.len, &m.fd, m.fd >= 0 ? 1 : 0)) {
      no_buffers_ = errno == ENOBUFS || errno == ENOMEM;
      return SendWouldBlock(errno);
    }
    msgs_.pop_front();
  }
  return true;
}

ssize_t ReceiveMessage(int sock, void* data, size_t cap, std::vector<UniqueFd>* fds) noexcept {
  struct iovec iov {};
  iov.iov_base = data;
  iov.iov_len = cap;
  alignas(struct cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * 8)];
  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctrl;
  msg.msg_controllen = sizeof(ctrl);
  ssize_t n;
  do {
    n = ::recvmsg(sock, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
  } while (n < 0 && errno == EINTR);
  if (n < 0) return -1;
  // At most 8 fds fit `ctrl`: with room for them reserved up front the
  // emplace_back below cannot throw. Without it the fds are closed.
  bool nomem = false;
  if (fds != nullptr) {
    try {
      fds->reserve(fds->size() + 8);
    } catch (...) {  // std::bad_alloc
      nomem = true;
    }
  }
  // musl's CMSG_NXTHDR compares size_t with ptrdiff_t (-Wsign-compare).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
  for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
    size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (size_t i = 0; i < count; ++i) {
      int fd;
      std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
      if (fds != nullptr && !nomem) {
        fds->emplace_back(fd);
      } else {
        ::close(fd);
      }
    }
  }
#pragma GCC diagnostic pop
  if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
    errno = EMSGSIZE;
    return -1;
  }
  if (nomem) {
    errno = ENOMEM;
    return -1;
  }
  return n;
}

bool GetPeerCredentials(int sock, PeerCredentials* out) noexcept {
  struct ucred cred {};
  socklen_t len = sizeof(cred);
  if (::getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return false;
  out->pid = cred.pid;
  out->uid = cred.uid;
  out->gid = cred.gid;
  return true;
}

int PidfdOpen(pid_t pid) noexcept {
  if (fault::no_pidfd.load(std::memory_order_relaxed)) {
    errno = ENOSYS;
    return -1;
  }
  return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
}

bool PeerInSupplementaryGroup(int sock, gid_t group) noexcept {
  // Enough for any sane group list; a peer in more groups than this is
  // checked by its effective gid only (the call fails with ERANGE).
  gid_t groups[256];
  socklen_t len = sizeof(groups);
  if (::getsockopt(sock, SOL_SOCKET, SO_PEERGROUPS, groups, &len) != 0) return false;
  for (size_t i = 0; i < len / sizeof(gid_t); ++i) {
    if (groups[i] == group) return true;
  }
  return false;
}

bool PeerGroupsSupported() noexcept {
  int sv[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return false;
  gid_t groups[256];
  socklen_t len = sizeof(groups);
  bool ok = ::getsockopt(sv[0], SOL_SOCKET, SO_PEERGROUPS, groups, &len) == 0;
  ::close(sv[0]);
  ::close(sv[1]);
  return ok;
}

int PidGone(pid_t pid) noexcept {
  if (fault::kill_eperm.load(std::memory_order_relaxed)) {
    errno = EPERM;
    return -1;
  }
  if (::kill(pid, 0) == 0) return 0;
  // Only "no such process" means dead. Anything else (EPERM for another
  // uid, EACCES from a security module, whatever a sandbox or another
  // kernel returns) proves nothing either way.
  return errno == ESRCH ? 1 : -1;
}

uint64_t Hash64(std::string_view s) noexcept {
  // FNV-1a; collisions are resolved by comparing the full URL at handshake.
  uint64_t h = 0xcbf29ce484222325ull;  // FNV offset basis
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

uint64_t NowNs() noexcept {
  struct timespec ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t RandomU64() noexcept {
  static thread_local std::mt19937_64 rng(std::random_device{}() ^ NowNs() ^
                                          static_cast<uint64_t>(::getpid()));
  return rng();
}

bool MakeDirectory(const std::string& path, std::optional<gid_t> group) noexcept {
  const mode_t mode = group ? (S_ISGID | 0770) : 0700;
  if (::mkdir(path.c_str(), mode & 0777) != 0 && errno != EEXIST) return false;
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) return false;
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return false;
  }
  if (st.st_uid != ::geteuid()) {
    // Shared group directory created by another member of the group. Its
    // creator controls the mode: anyone it lets in could plant or remove
    // sockets, so others must have no access and the group must be able
    // to create sockets. Membership may be supplementary (access() checks
    // with every group of the process).
    if (!group.has_value() || st.st_gid != *group || (st.st_mode & 0007) != 0 || (st.st_mode & 0070) != 0070 ||
        ::access(path.c_str(), R_OK | W_OK | X_OK) != 0) {
      errno = EACCES;
      return false;
    }
    return true;
  }
  // chown() to a group we are only a supplementary member of is allowed. It
  // comes first: chmod() drops S_ISGID unless the group is one of ours.
  if (group.has_value() && st.st_gid != *group && ::chown(path.c_str(), static_cast<uid_t>(-1), *group) != 0) {
    return false;
  }
  return ::chmod(path.c_str(), mode) == 0;
}

}  // namespace ibmw::selfdev::shm::detail
