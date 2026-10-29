// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Platform layer and wire format: memfd sealing, shared futex, fd passing,
// handle encoding, error names.

#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include <gtest/gtest.h>

#include "ibmw_selfdev/shm/shm.h"
#include "platform.h"
#include "protocol.h"

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
  for (int e = 0; e <= static_cast<int>(Error::kSystem); ++e) {
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

}  // namespace
}  // namespace ibmw::selfdev::shm
