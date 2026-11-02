// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Platform layer and wire format: memfd sealing, shared futex, fd passing,
// handle encoding, error names.

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ibmw_selfdev/shm/shm.h"
#include "platform.h"
#include "protocol.h"
#include "test_util.h"

namespace ibmw::selfdev::shm {
namespace {

using namespace detail;  // NOLINT

// PL-01 Sealed data segment: peers can only map it read-only, and they see
// the owner's writes (same physical pages, no copy).
TEST(ShmPlatformTest, SealedSegmentIsReadOnlyForPeers) {
  auto seg = CreateSegment("pl01", 4096, true, false);
  ASSERT_TRUE(seg.ok()) << ErrorName(seg.error());
  EXPECT_TRUE(seg->read_only_enforced);
  std::memcpy(seg->map.Data(), "hello", 6);

  auto rw = MapSegment(seg->fd.Get(), 4096, true);
  EXPECT_FALSE(rw.ok());  // F_SEAL_FUTURE_WRITE refuses new writable maps
  auto ro = MapSegment(seg->fd.Get(), 4096, false);
  ASSERT_TRUE(ro.ok());
  EXPECT_STREQ(reinterpret_cast<const char*>(ro->Data()), "hello");
  std::memcpy(seg->map.Data(), "world", 6);
  EXPECT_STREQ(reinterpret_cast<const char*>(ro->Data()), "world");

  // Growing or shrinking a sealed segment is refused too.
  EXPECT_NE(::ftruncate(seg->fd.Get(), 8192), 0);
  EXPECT_NE(::ftruncate(seg->fd.Get(), 1024), 0);
}

// PL-02 Unsealed (control) segments stay writable for the peer.
TEST(ShmPlatformTest, UnsealedSegmentIsWritable) {
  auto seg = CreateSegment("pl02", 4096, false, true);
  ASSERT_TRUE(seg.ok());
  auto rw = MapSegment(seg->fd.Get(), 4096, true);
  ASSERT_TRUE(rw.ok());
  rw->Data()[10] = 42;
  EXPECT_EQ(seg->map.Data()[10], 42);
}

// PL-03 A peer announcing the wrong size is rejected (fstat check).
TEST(ShmPlatformTest, MapSegmentRejectsSizeMismatch) {
  auto seg = CreateSegment("pl03", 4096, true, false);
  ASSERT_TRUE(seg.ok());
  EXPECT_FALSE(MapSegment(seg->fd.Get(), 8192, false).ok());
  EXPECT_FALSE(MapSegment(seg->fd.Get(), 0, false).ok());
  EXPECT_FALSE(MapSegment(-1, 4096, false).ok());
}

// PL-04 Futex wait honours the timeout and the expected value.
TEST(ShmPlatformTest, FutexTimeoutAndValueMismatch) {
  std::atomic<uint32_t> word{5};
  auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(FutexWait(&word, 5, 20'000'000));
  EXPECT_GE(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(15));
  // Value already changed: returns immediately.
  t0 = std::chrono::steady_clock::now();
  FutexWait(&word, 4, 1'000'000'000);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(500));
}

// PL-05 Futex wake on a word living in a shared memfd mapping, reached
// through two different virtual addresses (as across processes).
TEST(ShmPlatformTest, FutexWakeThroughSharedMapping) {
  auto seg = CreateSegment("pl05", 4096, false, true);
  ASSERT_TRUE(seg.ok());
  auto other = MapSegment(seg->fd.Get(), 4096, true);
  ASSERT_TRUE(other.ok());
  ASSERT_NE(other->Data(), seg->map.Data());
  auto* a = reinterpret_cast<std::atomic<uint32_t>*>(seg->map.Data());
  auto* b = reinterpret_cast<std::atomic<uint32_t>*>(other->Data());
  a->store(0);
  std::atomic<bool> woke{false};
  std::thread waiter([&] {
    while (b->load() == 0) FutexWait(b, 0, -1);
    woke = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(woke.load());
  a->store(1);
  FutexWake(a, 1);
  waiter.join();
  EXPECT_TRUE(woke.load());
}

// PL-06 SEQPACKET messages carry fds and peer credentials.
TEST(ShmPlatformTest, FdPassingAndCredentials) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv), 0);
  UniqueFd s0(sv[0]), s1(sv[1]);
  auto seg = CreateSegment("pl06", 4096, true, false);
  ASSERT_TRUE(seg.ok());
  seg->map.Data()[0] = 7;
  const char msg[] = "segment";
  int fd = seg->fd.Get();
  ASSERT_TRUE(SendMessage(s0.Get(), msg, sizeof(msg), &fd, 1));

  char buf[64] = {};
  std::vector<UniqueFd> fds;
  ASSERT_EQ(ReceiveMessage(s1.Get(), buf, sizeof(buf), &fds), static_cast<ssize_t>(sizeof(msg)));
  EXPECT_STREQ(buf, "segment");
  ASSERT_EQ(fds.size(), 1u);
  auto m = MapSegment(fds[0].Get(), 4096, false);
  ASSERT_TRUE(m.ok());
  EXPECT_EQ(m->Data()[0], 7);

  // Empty socket: -1/EAGAIN, not 0.
  fds.clear();
  EXPECT_EQ(ReceiveMessage(s1.Get(), buf, sizeof(buf), &fds), -1);
  EXPECT_EQ(errno, EAGAIN);

  PeerCredentials pc;
  ASSERT_TRUE(GetPeerCredentials(s1.Get(), &pc));
  EXPECT_EQ(pc.pid, ::getpid());
  EXPECT_EQ(pc.uid, ::geteuid());

  // Orderly shutdown reads as 0.
  s0.Reset();
  EXPECT_EQ(ReceiveMessage(s1.Get(), buf, sizeof(buf), &fds), 0);
}

// PL-07 Handle layout: 12-bit segment, 20-bit index, 32-bit generation.
TEST(ShmPlatformTest, HandleRoundTrip) {
  for (uint32_t seg : {0u, 1u, 4095u}) {
    for (uint32_t idx : {0u, 77u, (1u << 20) - 1}) {
      for (uint32_t gen : {0u, 1u, 0xffffffffu}) {
        uint64_t h = MakeHandle(seg, idx, gen);
        EXPECT_EQ(HandleSegment(h), seg);
        EXPECT_EQ(HandleIndex(h), idx);
        EXPECT_EQ(HandleGeneration(h), gen);
      }
    }
  }
  static_assert(kMaxSegmentsPerPublisher <= (1u << 12));
  static_assert(kMaxChunksPerSegment <= (1u << 20));
}

// PL-08 Shared layouts are fixed size and cache-line separated.
TEST(ShmPlatformTest, WireLayout) {
  EXPECT_EQ(sizeof(ChunkHeader), 64u);
  EXPECT_EQ(offsetof(ChunkHeader, back_offset), 60u);
  EXPECT_EQ(sizeof(NotifySlot), 64u);
  EXPECT_EQ(offsetof(ConnHeader, delivery_read) - offsetof(ConnHeader, delivery_write), 64u);
  EXPECT_LE(sizeof(HelloMsg), kMaxMsgSize);
  EXPECT_TRUE(std::atomic<uint64_t>::is_always_lock_free);
  EXPECT_TRUE(std::atomic<uint32_t>::is_always_lock_free);
}

// PL-09 Error names and Result.
TEST(ShmPlatformTest, ResultAndErrorNames) {
  Result<int> ok(3);
  EXPECT_TRUE(ok.ok());
  EXPECT_EQ(*ok, 3);
  Result<int> bad(Error::kNoMemory);
  EXPECT_FALSE(bad);
  EXPECT_EQ(bad.error(), Error::kNoMemory);
  EXPECT_STREQ(ErrorName(Error::kTooManyLoans), "too_many_loans");
  for (int e = 0; e <= static_cast<int>(Error::kAlreadyInitialized); ++e) {
    EXPECT_STRNE(ErrorName(static_cast<Error>(e)), "unknown");
  }
  Result<void> v;
  EXPECT_TRUE(v.ok());
}

// PL-10 URL hash is stable (it names sockets shared across processes and
// builds, so it must never change silently).
TEST(ShmPlatformTest, HashIsStable) {
  EXPECT_EQ(Hash64(""), 0xcbf29ce484222325ull);  // FNV-1a offset basis
  EXPECT_EQ(Hash64("a"), 0xaf63dc4c8601ec8cull);
  EXPECT_NE(Hash64("/a/b"), Hash64("/a/c"));
}

// PL-11 (third review M5) Received segments must be tmpfs (memfd) files.
// A sealed hugetlbfs memfd passes every other check but cannot be mapped
// safely (SIGBUS/ENOMEM when the huge page pool runs dry): rejected.
TEST(ShmPlatformTest, MapSegmentRequiresTmpfs) {
  auto seg = CreateSegment("pl11", 4096, true, false);
  ASSERT_TRUE(seg.ok());
  EXPECT_TRUE(MapSegment(seg->fd.Get(), 4096, false).ok());

  constexpr size_t kHuge = 2u << 20;
  UniqueFd huge(static_cast<int>(
      ::syscall(SYS_memfd_create, "pl11_huge", MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_HUGETLB)));
  bool have_huge = huge.Valid() && ::ftruncate(huge.Get(), kHuge) == 0 &&
                   ::fcntl(huge.Get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0;
  RecordProperty("hugetlb_memfd", have_huge ? "yes" : "no");
  if (have_huge) {
    auto m = MapSegment(huge.Get(), kHuge, false);
    ASSERT_FALSE(m.ok());
    EXPECT_EQ(m.error(), Error::kCorrupted);
  }
  // Whatever the kernel offers, a non-memfd file is never accepted.
  std::string path = test::TestRoot() + "/pl11_XXXXXX";
  UniqueFd file(::mkstemp(path.data()));
  ASSERT_TRUE(file.Valid()) << std::strerror(errno);
  ::unlink(path.c_str());
  ASSERT_EQ(::ftruncate(file.Get(), 4096), 0);
  EXPECT_FALSE(MapSegment(file.Get(), 4096, false).ok());
}

// Fills `sock` until the next send would block; returns how many fit.
size_t FillSocket(int sock) {
  size_t n = 0;
  for (uint64_t v = 0;; ++v) {
    if (!SendMessage(sock, &v, sizeof(v), nullptr, 0)) {
      EXPECT_TRUE(SendWouldBlock(errno)) << std::strerror(errno);
      return n;
    }
    ++n;
  }
}

// PL-12 (third review H1/M6) Messages a full socket cannot take are queued
// and sent in order on Flush(); an owned fd is closed once its message has
// gone; the queue is bounded.
TEST(ShmPlatformTest, SendQueueOrderOwnershipAndBound) {
  int sv[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv), 0);
  UniqueFd s0(sv[0]), s1(sv[1]);
  const size_t filled = FillSocket(s0.Get());
  ASSERT_GT(filled, 0u);

  SendQueue q;
  int pfd[2];
  ASSERT_EQ(::pipe2(pfd, O_CLOEXEC), 0);
  UniqueFd pipe_r(pfd[0]);
  for (uint64_t v = 1000; v < 1010; ++v) {
    UniqueFd owned = v == 1005 ? UniqueFd(pfd[1]) : UniqueFd();
    EXPECT_EQ(q.Send(s0.Get(), &v, sizeof(v), -1, std::move(owned)), SendQueue::Status::kQueued);
  }
  EXPECT_FALSE(q.Empty());
  EXPECT_TRUE(q.Flush(s0.Get()));  // still full: nothing sent, nothing lost
  EXPECT_FALSE(q.Empty());

  std::vector<uint64_t> got;
  for (int round = 0; round < 1000 && got.size() < filled + 10; ++round) {
    char buf[64];
    std::vector<UniqueFd> fds;
    ssize_t n;
    while ((n = ReceiveMessage(s1.Get(), buf, sizeof(buf), &fds)) > 0) {
      uint64_t v;
      std::memcpy(&v, buf, sizeof(v));
      got.push_back(v);
      fds.clear();  // the received copy of the pipe end is closed here
    }
    ASSERT_TRUE(q.Flush(s0.Get()));
  }
  ASSERT_EQ(got.size(), filled + 10);
  EXPECT_TRUE(q.Empty());
  for (size_t i = 0; i < filled; ++i) EXPECT_EQ(got[i], i);
  for (size_t i = 0; i < 10; ++i) EXPECT_EQ(got[filled + i], 1000 + i);
  // Every write end is closed now: the queue released its owned copy.
  char c;
  EXPECT_EQ(::read(pipe_r.Get(), &c, 1), 0);

  // Bound: a peer that never reads cannot grow the queue without limit.
  ASSERT_GT(FillSocket(s0.Get()), 0u);
  SendQueue bounded;
  size_t queued = 0;
  for (uint64_t v = 0; v < SendQueue::kMaxMessages; ++v) {
    queued += bounded.Send(s0.Get(), &v, sizeof(v), -1) == SendQueue::Status::kQueued ? 1 : 0;
  }
  EXPECT_EQ(queued, SendQueue::kMaxMessages);
  uint64_t one_more = 1;
  EXPECT_EQ(bounded.Send(s0.Get(), &one_more, sizeof(one_more), -1), SendQueue::Status::kOverflow);

  // A broken connection is reported as such.
  s1.Reset();
  SendQueue broken;
  EXPECT_EQ(broken.Send(s0.Get(), &one_more, sizeof(one_more), -1), SendQueue::Status::kFailed);
}

// PL-13 (third review H1) Peer death detection. A zombie still exists for
// kill(), so PidGone() only reports it once reaped; a pidfd is readable as
// soon as the process exits. pidfd may be missing (old kernel, sandbox).
TEST(ShmPlatformTest, PeerDeathDetection) {
  pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    ::pause();
    ::_exit(0);
  }
  struct Reap {
    pid_t pid;
    ~Reap() {
      if (pid > 0) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
      }
    }
  } reap{child};
  EXPECT_EQ(PidGone(child), 0);
  UniqueFd pidfd(PidfdOpen(child));
  RecordProperty("pidfd", pidfd.Valid() ? "yes" : "no");
  if (!pidfd.Valid()) {
    EXPECT_TRUE(errno == ENOSYS || errno == EPERM || errno == EACCES) << std::strerror(errno);
  } else {
    struct pollfd p {pidfd.Get(), POLLIN, 0};
    EXPECT_EQ(::poll(&p, 1, 0), 0);  // alive: not readable
  }
  ASSERT_EQ(::kill(child, SIGKILL), 0);
  if (pidfd.Valid()) {
    struct pollfd p {pidfd.Get(), POLLIN, 0};
    EXPECT_EQ(::poll(&p, 1, 5000), 1);  // exited, not yet reaped
  } else {
    ::usleep(50000);
  }
  EXPECT_EQ(PidGone(child), 0);  // a zombie still answers kill(pid, 0)
  ASSERT_EQ(::waitpid(child, nullptr, 0), child);
  reap.pid = -1;
  EXPECT_EQ(PidGone(child), 1);

  fault::no_pidfd.store(true);
  EXPECT_EQ(PidfdOpen(::getpid()), -1);
  EXPECT_EQ(errno, ENOSYS);
  fault::no_pidfd.store(false);
}

}  // namespace
}  // namespace ibmw::selfdev::shm
