// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "platform.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002
#endif
#ifndef F_SEAL_GROW
#define F_SEAL_GROW 0x0004
#endif
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

namespace ibmw::selfdev::shm::detail {

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
  if (seal_write && ::fcntl(seg.fd.Get(), F_ADD_SEALS, seals | F_SEAL_FUTURE_WRITE) == 0) {
    seg.read_only_enforced = true;
  } else if (::fcntl(seg.fd.Get(), F_ADD_SEALS, seals) != 0) {
    return Error::kSystem;
  }
  return seg;
}

Result<Mapping> MapSegment(int fd, size_t size, bool writable) {
  struct stat st {};
  if (::fstat(fd, &st) != 0) return Error::kSystem;
  if (size == 0 || static_cast<uint64_t>(st.st_size) < size) return Error::kCorrupted;
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
  if (nfds > 4) return false;
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
    return false;
  }
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
  for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
    size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    for (size_t i = 0; i < count; ++i) {
      int fd;
      std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
      if (fds != nullptr) {
        fds->emplace_back(fd);
      } else {
        ::close(fd);
      }
    }
  }
  if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
    errno = EMSGSIZE;
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

bool MakeDirectory(const std::string& path, mode_t mode, std::optional<gid_t> group) noexcept {
  if (::mkdir(path.c_str(), mode) != 0 && errno != EEXIST) return false;
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
  if (st.st_uid != ::geteuid()) {
    // Shared group directory created by another user of the same group.
    return group.has_value() && st.st_gid == *group;
  }
  if (group.has_value() && st.st_gid != *group && ::chown(path.c_str(), static_cast<uid_t>(-1), *group) != 0) {
    return false;
  }
  return ::chmod(path.c_str(), mode) == 0;
}

}  // namespace ibmw::selfdev::shm::detail
