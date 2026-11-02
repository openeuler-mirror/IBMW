// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// The membarrier(2) side of BoundedBlockingQueue. Its own executable, run
// twice by ctest: as is, and with IBMW_SELFDEV_NO_MEMBARRIER=1, which is
// read once per process.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#include "ibmw_selfdev/concurrent/bounded_blocking_queue.h"

namespace {

using ibmw::selfdev::concurrent::BoundedBlockingQueue;
namespace detail = ibmw::selfdev::concurrent::detail;

bool EnvOptedOut() {
  const char* off = std::getenv("IBMW_SELFDEV_NO_MEMBARRIER");
  return off != nullptr && off[0] != '\0' && std::strcmp(off, "0") != 0;
}

// From now on membarrier(2) fails with EPERM in this process, as under a
// sandbox that forbids it.
void BlockMembarrier() {
  sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_membarrier, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog prog{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
    std::perror("seccomp");
    std::_Exit(2);
  }
}

// Exact multiset through the queue, 4 producers and 4 consumers.
void Mpmc() {
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 20000;
  BoundedBlockingQueue<int> q(64);
  std::vector<std::thread> threads;
  std::vector<int64_t> sums(kProducers, 0);
  for (int p = 0; p < kProducers; ++p) {
    threads.emplace_back([&q] {
      for (int i = 1; i <= kPerProducer; ++i) ASSERT_TRUE(q.Push(i));
    });
    threads.emplace_back([&q, &sums, p] {
      int v = 0;
      for (int i = 0; i < kPerProducer; ++i) {
        ASSERT_TRUE(q.Pop(v));
        sums[p] += v;
      }
    });
  }
  for (auto& t : threads) t.join();
  int64_t total = 0;
  for (int64_t s : sums) total += s;
  EXPECT_EQ(total, int64_t{kProducers} * kPerProducer * (kPerProducer + 1) / 2);
  EXPECT_TRUE(q.Empty());
}

// BQ-36: with the variable set, or built with IBMW_SELFDEV_NO_MEMBARRIER,
// the queue runs on fences and never registers.
TEST(BoundedBlockingQueueBarrierTest, OptOutUsesFences) {
  BoundedBlockingQueue<int> q;
  const bool registered = detail::RegisterMembarrier();
  EXPECT_EQ(registered, detail::membarrier_ready.load());
  if (EnvOptedOut() || !IBMW_SELFDEV_HAS_MEMBARRIER) {
    EXPECT_FALSE(registered);
    EXPECT_FALSE(detail::membarrier_ready.load());
  }
  Mpmc();
}

// BQ-37: membarrier(2) blocked after registration aborts with a message
// instead of losing wakeups; without registration the fence keeps working.
TEST(BoundedBlockingQueueBarrierDeathTest, BlockedAfterRegistrationAborts) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  if (detail::RegisterMembarrier()) {
    EXPECT_DEATH(
        {
          BoundedBlockingQueue<int> q;
          BlockMembarrier();
          detail::HeavyBarrier();
        },
        "membarrier.*failed after registration.*IBMW_SELFDEV_NO_MEMBARRIER=1");
  } else {
    EXPECT_EXIT(
        {
          BlockMembarrier();
          Mpmc();
          detail::HeavyBarrier();
          std::exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0), "");
  }
}

}  // namespace
