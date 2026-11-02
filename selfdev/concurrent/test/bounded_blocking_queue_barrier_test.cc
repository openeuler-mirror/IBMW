// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// The membarrier(2) side of BoundedBlockingQueue: the opt-out, and failures
// after registration. Its own executable, run twice by ctest: as is, and
// with IBMW_SELFDEV_NO_MEMBARRIER=1, which is read once per process.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ibmw_selfdev/concurrent/bounded_blocking_queue.h"

namespace {

using ibmw::selfdev::concurrent::BoundedBlockingQueue;
namespace detail = ibmw::selfdev::concurrent::detail;

bool EnvOptedOut() {
  const char* off = std::getenv("IBMW_SELFDEV_NO_MEMBARRIER");
  return off != nullptr && off[0] != '\0' && std::strcmp(off, "0") != 0;
}

// Runs probe() in a child process; whether it exited with 0 (a probe that
// hangs is ended by an alarm after 10 s). Death tests fork after it, so the
// filters it installs stay in the child.
bool ProbeInChild(int (*probe)()) {
  std::fflush(nullptr);
  const pid_t pid = fork();
  if (pid == 0) {
    alarm(10);
    std::_Exit(probe());
  }
  int status = 0;
  return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// seccomp filters (Linux 3.5, CONFIG_SECCOMP_FILTER), used by BlockMembarrier().
bool SeccompFilters() {
  static const bool ok = ProbeInChild([] {
    sock_filter filter[] = {BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)};
    sock_fprog prog{1, filter};
    return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0 ? 0
                                                                                                             : 1;
  });
  return ok;
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

// From now on membarrier(2) calls of this thread (and threads it creates)
// stop until a supervisor answers them on the returned descriptor (seccomp
// user notification).
int SuperviseMembarrier() {
  sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_membarrier, 0, 1),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  sock_fprog prog{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
  const long fd = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0
                      ? syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog)
                      : -1;
  if (fd < 0) {
    std::perror("seccomp");
    std::_Exit(2);
  }
  return static_cast<int>(fd);
}

// seccomp user notification with SECCOMP_USER_NOTIF_FLAG_CONTINUE (Linux
// 5.5), used by SuperviseMembarrier() and Answer(): a supervised getppid(2)
// of another thread is let run.
bool UserNotifContinue() {
  static const bool ok = ProbeInChild([] {
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_USER_NOTIF),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog prog{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 1;
    const long fd = syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &prog);
    if (fd < 0) return 1;
    std::atomic<long> parent{0};
    std::thread caller([&] { parent = syscall(SYS_getppid); });
    seccomp_notif req;
    std::memset(&req, 0, sizeof(req));
    seccomp_notif_resp resp;
    std::memset(&resp, 0, sizeof(resp));
    if (ioctl(static_cast<int>(fd), SECCOMP_IOCTL_NOTIF_RECV, &req) != 0) std::_Exit(1);
    resp.id = req.id;
    resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
    if (ioctl(static_cast<int>(fd), SECCOMP_IOCTL_NOTIF_SEND, &resp) != 0) std::_Exit(1);
    caller.join();
    return parent.load() > 0 ? 0 : 1;
  });
  return ok;
}

// Answers the next membarrier(2) call: fails it with `error`, or with 0
// lets it run.
void Answer(int fd, int error) {
  seccomp_notif req;
  std::memset(&req, 0, sizeof(req));
  seccomp_notif_resp resp;
  std::memset(&resp, 0, sizeof(resp));
  if (ioctl(fd, SECCOMP_IOCTL_NOTIF_RECV, &req) != 0) {
    std::perror("SECCOMP_IOCTL_NOTIF_RECV");
    std::_Exit(2);
  }
  resp.id = req.id;
  if (error != 0) {
    resp.error = -error;
  } else {
    resp.flags = SECCOMP_USER_NOTIF_FLAG_CONTINUE;
  }
  if (ioctl(fd, SECCOMP_IOCTL_NOTIF_SEND, &resp) != 0) {
    std::perror("SECCOMP_IOCTL_NOTIF_SEND");
    std::_Exit(2);
  }
}

long VoluntarySwitches(long tid) {
  std::ifstream in("/proc/self/task/" + std::to_string(tid) + "/status");
  for (std::string line; std::getline(in, line);) {
    if (line.rfind("voluntary_ctxt_switches:", 0) == 0) return std::stol(line.substr(line.find(':') + 1));
  }
  return -1;
}

// A TryPop waits (as a front sleeper) for an item whose producer stays in
// its constructor for 200 ms; returns how often it woke meanwhile.
long FrontWaitWakeups() {
  struct Gated {
    int v = -1;
    Gated() = default;
    Gated(int x, std::atomic<bool>* gate) : v(x) {
      while (!gate->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Gated(Gated&& o) noexcept : v(o.v) {}
    Gated& operator=(Gated&& o) noexcept {
      v = o.v;
      return *this;
    }
  };
  BoundedBlockingQueue<Gated> q;
  std::atomic<bool> gate{false};
  std::thread producer([&] { EXPECT_TRUE(q.Emplace(7, &gate)); });
  while (q.Size() == 0) std::this_thread::yield();
  std::atomic<long> tid{0};
  std::atomic<int> got{-1};
  std::thread consumer([&] {
    tid = syscall(SYS_gettid);
    Gated out;
    got = q.TryPop(out) ? out.v : -2;
  });
  while (tid.load() == 0) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));  // past its spins and yields
  const long before = VoluntarySwitches(tid);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const long woke = VoluntarySwitches(tid) - before;
  gate = true;
  producer.join();
  consumer.join();
  EXPECT_EQ(got.load(), 7);
  return woke;
}

// BQ-37: membarrier(2) failing after registration (here EPERM from a
// seccomp filter installed later) makes HeavyBarrier() report false, once
// with a message, and the queue keeps working: a thread waiting at a lane
// re-checks every millisecond instead of relying on a wakeup. Without
// registration the fence keeps working.
TEST(BoundedBlockingQueueBarrierDeathTest, BlockedAfterRegistrationKeepsWorking) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  if (!SeccompFilters()) GTEST_SKIP() << "this kernel has no seccomp filters (Linux 3.5, CONFIG_SECCOMP_FILTER)";
  if (detail::RegisterMembarrier()) {
    EXPECT_EXIT(
        {
          BoundedBlockingQueue<int> q;
          BlockMembarrier();
          EXPECT_FALSE(detail::HeavyBarrier());
          EXPECT_FALSE(detail::HeavyBarrier());
          Mpmc();
          EXPECT_GE(FrontWaitWakeups(), 20);
          std::exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0),
        // One line: reported once.
        "^ibmw_selfdev: membarrier[^\n]*failed after registration: Operation not permitted; [^\n]*re-check"
        "[^\n]*IBMW_SELFDEV_NO_MEMBARRIER=1[^\n]*\n$");
  } else {
    EXPECT_EXIT(
        {
          BlockMembarrier();
          Mpmc();
          EXPECT_TRUE(detail::HeavyBarrier());
          std::exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0), "^$");
  }
}

// Five heavy barriers; each membarrier(2) call is answered by a supervisor
// thread, which takes exactly as many calls as expected (one too many
// blocks the barrier until the alarm ends the process).
void RetrySequence() {
  const int fd = SuperviseMembarrier();
  std::vector<int> answers = {ENOMEM, EINTR, EAGAIN, 0};  // succeeds on the last try
  for (int n = 0; n < detail::kMembarrierTries; ++n) answers.push_back(ENOMEM);  // gives up
  answers.push_back(0);      // but tries again next time
  answers.push_back(EPERM);  // not retried, and never tried again
  std::thread supervisor([&] {
    for (int error : answers) Answer(fd, error);
  });
  EXPECT_TRUE(detail::HeavyBarrier());
  EXPECT_FALSE(detail::HeavyBarrier());
  EXPECT_TRUE(detail::HeavyBarrier());
  EXPECT_FALSE(detail::HeavyBarrier());
  supervisor.join();
  EXPECT_FALSE(detail::HeavyBarrier());  // no system call left to answer
}

// BQ-39: transient failures (ENOMEM: the kernel's CPU mask allocation,
// EINTR, EAGAIN) are retried up to kMembarrierTries times in all, and the
// next barrier tries again; other errors are not retried, and later
// barriers skip the system call.
TEST(BoundedBlockingQueueBarrierDeathTest, TransientFailuresRetried) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  if (!SeccompFilters()) GTEST_SKIP() << "this kernel has no seccomp filters (Linux 3.5, CONFIG_SECCOMP_FILTER)";
  if (!detail::RegisterMembarrier()) {
    // Opted out: membarrier(2) is never called.
    EXPECT_EXIT(
        {
          BlockMembarrier();
          EXPECT_TRUE(detail::HeavyBarrier());
          std::exit(::testing::Test::HasFailure() ? 1 : 0);
        },
        ::testing::ExitedWithCode(0), "^$");
    return;
  }
  if (!UserNotifContinue()) {
    GTEST_SKIP() << "this kernel cannot let a supervised system call run "
                    "(seccomp SECCOMP_USER_NOTIF_FLAG_CONTINUE, Linux 5.5)";
  }
  EXPECT_EXIT(
      {
        alarm(30);  // a call too many (or too few) blocks: fail instead of hanging
        RetrySequence();
        std::exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "failed after registration: Cannot allocate memory");
}

}  // namespace
