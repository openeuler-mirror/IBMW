// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// ibmw_shm benchmark: cross-process round-trip latency (busy-poll and
// listener/futex wake-up), 1:1 throughput, idle CPU, and the memcpy cost a
// copying transport would add. Spawns itself as the echo process.
//
//   ibmw_shm_bench [--iterations N] [--sizes 64,4096,...] [--quick]

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ibmw_selfdev/shm/shm.h"

using namespace ibmw::selfdev::shm;  // NOLINT
using Clock = std::chrono::steady_clock;

namespace {

struct Msg {
  uint64_t seq;
  uint32_t size;
  uint32_t stop;
};

uint64_t NowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

double CpuSeconds() {
  struct rusage ru {};
  ::getrusage(RUSAGE_SELF, &ru);
  return static_cast<double>(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) +
         static_cast<double>(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
}

std::string Url(const char* what) { return std::string("/bench/") + what + "/" + std::to_string(::getppid()); }

// ---------------------------------------------------------------- echo side

struct Echo {
  Publisher* pub;
  std::atomic<bool> stop{false};
};

void EchoOne(Subscriber* sub, Echo* e) {
  while (auto t = sub->Take()) {
    const auto* in = static_cast<const Msg*>(*t);
    if (in->stop != 0) e->stop = true;
    auto l = e->pub->Loan(in->size);
    if (l) {
      std::memcpy(*l, in, sizeof(Msg));  // header only: the payload is not copied
      e->pub->Publish(*l);
    }
    sub->Release(*t);
  }
}

int RunEcho(bool use_listener) {
  // Echo process: subscribe to "ping", answer on "pong".
  std::string base = std::to_string(::getppid());
  Publisher pub("/bench/pong/" + base);
  Subscriber sub("/bench/ping/" + base, SubscriberOptions{4, QueueFullPolicy::kDiscardOldest, 4});
  Echo e{&pub};
  std::printf("ready\n");
  std::fflush(stdout);
  if (use_listener) {
    Listener l;
    l.Attach(sub, [](Subscriber* s, void* p) { EchoOne(s, static_cast<Echo*>(p)); }, &e);
    while (!e.stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } else {
    while (!e.stop.load()) EchoOne(&sub, &e);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return 0;
}

// ---------------------------------------------------------------- ping side

struct Stats {
  double p50, p99, p999, max, mean;
};

Stats Summarize(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))]; };
  double sum = 0;
  for (double x : v) sum += x;
  return {at(0.5), at(0.99), at(0.999), v.back(), sum / static_cast<double>(v.size())};
}

pid_t SpawnEcho(const char* self, bool listener, int* out_fd) {
  int fds[2];
  if (::pipe(fds) != 0) std::exit(1);
  pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    ::close(fds[0]);
    ::execl(self, self, listener ? "--echo-listener" : "--echo-poll", nullptr);
    ::_exit(127);
  }
  ::close(fds[1]);
  char buf[16] = {};
  if (::read(fds[0], buf, 6) != 6) std::exit(1);
  *out_fd = fds[0];
  return pid;
}

struct Waiter {
  std::atomic<uint32_t> ready{0};
};

void RunLatency(const char* self, bool listener, const std::vector<uint32_t>& sizes, int iterations) {
  std::string base = std::to_string(::getpid());
  Publisher ping("/bench/ping/" + base);
  Subscriber pong("/bench/pong/" + base, SubscriberOptions{4, QueueFullPolicy::kDiscardOldest, 4});
  int fd = -1;
  pid_t child = SpawnEcho(self, listener, &fd);
  Waiter w;
  Listener l;
  if (listener) {
    l.Attach(pong, [](Subscriber*, void* p) {
      auto* x = static_cast<Waiter*>(p);
      x->ready.store(1);
      x->ready.notify_one();
    }, &w);
  }
  auto deadline = Clock::now() + std::chrono::seconds(10);
  while ((!ping.HasSubscribers() || pong.Stats().connections == 0) && Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::printf("\n%s round trip / 2 (one-way latency, us), cross-process:\n", listener ? "LISTENER (futex wake)" : "BUSY-POLL");
  std::printf("  %10s %8s %8s %8s %8s %8s %8s %6s\n", "size", "mean", "p50", "p99", "p99.9", "max", "memcpy", "lost");
  for (uint32_t size : sizes) {
    int n = size >= (1u << 20) ? std::max(200, iterations / 20) : iterations;
    std::vector<double> lat;
    lat.reserve(static_cast<size_t>(n));
    int lost = 0;
    for (int i = -100; i < n; ++i) {  // 100 warm-up rounds
      auto loan = ping.Loan(size);
      if (!loan) {
        ++lost;
        continue;
      }
      auto* m = static_cast<Msg*>(*loan);
      m->seq = static_cast<uint64_t>(i + 1000);
      m->size = size;
      m->stop = 0;
      w.ready.store(0);
      uint64_t t0 = NowNs();
      ping.Publish(*loan);
      bool got = false;
      auto until = Clock::now() + std::chrono::seconds(1);
      while (!got && Clock::now() < until) {
        if (listener && !pong.HasData()) w.ready.wait(0);
        if (listener) w.ready.store(0);
        while (auto t = pong.Take()) {
          got = got || static_cast<const Msg*>(*t)->seq == m->seq;
          pong.Release(*t);
        }
      }
      uint64_t t1 = NowNs();
      if (!got) {
        ++lost;
        continue;
      }
      if (i >= 0) lat.push_back(static_cast<double>(t1 - t0) / 2000.0);
    }
    // What one copy of the payload costs (a copying transport pays >= 2).
    std::vector<uint8_t> a(size, 1), b(size);
    uint64_t c0 = NowNs();
    for (int k = 0; k < 50; ++k) std::memcpy(b.data(), a.data(), size);
    double copy_us = static_cast<double>(NowNs() - c0) / 50.0 / 1000.0;
    auto s = Summarize(lat);
    std::printf("  %10u %8.2f %8.2f %8.2f %8.2f %8.2f %8.2f %6d\n", size, s.mean, s.p50, s.p99, s.p999, s.max, copy_us, lost);
  }
  auto loan = ping.Loan(sizeof(Msg));
  if (loan) {
    auto* m = static_cast<Msg*>(*loan);
    *m = Msg{1, sizeof(Msg), 1};
    ping.Publish(*loan);
  }
  int status = 0;
  ::waitpid(child, &status, 0);
  ::close(fd);
}

void RunThroughput(uint32_t size, double seconds) {
  std::string url = Url("tput") + "_" + std::to_string(size);
  Publisher pub(url);
  Subscriber sub(url, SubscriberOptions{256, QueueFullPolicy::kDiscardOldest, 16});
  while (!pub.HasSubscribers()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::atomic<bool> done{false};
  uint64_t received = 0;
  std::thread consumer([&] {
    while (!done.load() || sub.HasData()) {
      if (auto t = sub.Take()) {
        ++received;
        sub.Release(*t);
      }
    }
  });
  uint64_t sent = 0;
  auto end = Clock::now() + std::chrono::duration<double>(seconds);
  while (Clock::now() < end) {
    auto l = pub.Loan(size);
    if (!l) continue;
    static_cast<uint8_t*>(*l)[0] = 1;
    pub.Publish(*l);
    ++sent;
  }
  done = true;
  consumer.join();
  double rate = static_cast<double>(sent) / seconds;
  std::printf("  %10u %12.0f msg/s %10.2f GB/s logical  received %.1f%%\n", size, rate,
              rate * size / 1e9, 100.0 * static_cast<double>(received) / static_cast<double>(std::max<uint64_t>(sent, 1)));
}

void RunIdleCpu(double seconds) {
  std::string url = Url("idle");
  Publisher pub(url);
  Subscriber sub(url);
  Listener l;
  l.Attach(sub, [](Subscriber*, void*) {}, nullptr);
  while (!pub.HasSubscribers()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  double c0 = CpuSeconds();
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  double used = CpuSeconds() - c0;
  std::printf("\nIDLE: 1 publisher + 1 subscriber + listener for %.1fs: CPU %.3f ms (%.4f%%)\n", seconds, used * 1e3,
              100.0 * used / seconds);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--echo-poll") == 0) return RunEcho(false);
  if (argc > 1 && std::strcmp(argv[1], "--echo-listener") == 0) return RunEcho(true);
  int iterations = 10000;
  std::vector<uint32_t> sizes = {64, 1024, 4096, 65536, 1u << 20, 4u << 20};
  double tput_seconds = 2.0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--iterations" && i + 1 < argc) iterations = std::atoi(argv[++i]);
    if (a == "--quick") {
      iterations = 2000;
      tput_seconds = 0.5;
    }
    if (a == "--sizes" && i + 1 < argc) {
      sizes.clear();
      std::string s = argv[++i];
      for (size_t p = 0; p < s.size();) {
        size_t q = s.find(',', p);
        sizes.push_back(static_cast<uint32_t>(std::strtoul(s.substr(p, q - p).c_str(), nullptr, 10)));
        p = q == std::string::npos ? s.size() : q + 1;
      }
    }
  }
  std::printf("ibmw_shm benchmark (pid %d, %u cpus)\n", ::getpid(), std::thread::hardware_concurrency());
  RunLatency(argv[0], false, sizes, iterations);
  RunLatency(argv[0], true, sizes, iterations);
  std::printf("\nTHROUGHPUT 1:1 same process, DISCARD_OLDEST queue 256, %.1fs:\n", tput_seconds);
  for (uint32_t s : {64u, 4096u, 1u << 20}) RunThroughput(s, tput_seconds);
  RunIdleCpu(2.0);
  return 0;
}
