// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Child process for multi-process tests. Talks to the test through stdout
// lines ("ready", "done <n>") and its exit code:
//   0 ok, 2 data mismatch, 3 timeout, 4 setup failure, 64 usage.
//
//   pub      <url> <count> <lossless> <size>  publish once a subscriber is there
//   sub      <url> <count> <lossless> <size>  receive and verify, in order if lossless
//   sub_hold <url> <n>                        take n chunks, keep them, wait to be killed
//   pub_hold <url>                            publish one chunk, wait to be killed

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "ibmw_selfdev/shm/shm.h"

using namespace ibmw::selfdev::shm;  // NOLINT

namespace {

void Say(const std::string& s) {
  std::printf("%s\n", s.c_str());
  std::fflush(stdout);
}

void Fill(void* payload, uint32_t size, uint64_t seq) {
  auto* p = static_cast<uint8_t*>(payload);
  std::memcpy(p, &seq, sizeof(seq));
  for (uint32_t i = sizeof(seq); i < size; ++i) p[i] = static_cast<uint8_t>(seq * 31 + i);
}

bool Check(const void* payload, uint32_t size, uint64_t* seq) {
  auto* p = static_cast<const uint8_t*>(payload);
  std::memcpy(seq, p, sizeof(*seq));
  for (uint32_t i = sizeof(*seq); i < size; ++i) {
    if (p[i] != static_cast<uint8_t>(*seq * 31 + i)) return false;
  }
  return true;
}

template <class Pred>
bool Wait(Pred pred, int ms) {
  for (int i = 0; i < ms; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

[[noreturn]] void Park() {
  for (;;) ::pause();
}

int RunPub(const std::string& url, uint64_t count, bool lossless, uint32_t size) {
  PublisherOptions po;
  po.too_slow = lossless ? TooSlowPolicy::kWaitForConsumer : TooSlowPolicy::kDiscardOldest;
  Publisher pub(url, po);
  if (!pub.Valid()) return 4;
  Say("ready");
  if (!Wait([&] { return pub.HasSubscribers(); }, 10000)) return 3;
  for (uint64_t seq = 1; seq <= count; ++seq) {
    auto loan = pub.Loan(size);
    if (!loan) {
      std::fprintf(stderr, "loan failed: %s\n", ErrorName(loan.error()));
      return 4;
    }
    Fill(*loan, size, seq);
    pub.Publish(*loan);
  }
  Say("done " + std::to_string(count));
  // Keep the publisher alive until the test reads everything or kills us.
  std::this_thread::sleep_for(std::chrono::seconds(30));
  return 0;
}

int RunSub(const std::string& url, uint64_t count, bool lossless, uint32_t size) {
  SubscriberOptions so;
  so.queue_capacity = lossless ? 8 : 256;
  so.queue_full = lossless ? QueueFullPolicy::kBlockProducer : QueueFullPolicy::kDiscardOldest;
  Subscriber sub(url, so);
  if (!sub.Valid()) return 4;
  Say("ready");
  uint64_t got = 0, last = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (lossless ? got < count : last < count) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "timeout: got %llu last %llu\n", static_cast<unsigned long long>(got),
                   static_cast<unsigned long long>(last));
      return 3;
    }
    auto t = sub.Take();
    if (!t) {
      std::this_thread::yield();
      continue;
    }
    uint64_t seq = 0;
    bool ok = ChunkView::FromUserPayload(*t).PayloadSize() == size && Check(*t, size, &seq);
    sub.Release(*t);
    if (!ok || seq <= last || (lossless && seq != last + 1)) {
      std::fprintf(stderr, "mismatch: seq %llu last %llu\n", static_cast<unsigned long long>(seq),
                   static_cast<unsigned long long>(last));
      return 2;
    }
    last = seq;
    ++got;
  }
  Say("done " + std::to_string(got));
  return 0;
}

int RunSubHold(const std::string& url, uint32_t n) {
  SubscriberOptions so;
  so.queue_capacity = 16;
  Subscriber sub(url, so);
  if (!sub.Valid()) return 4;
  uint32_t held = 0;
  if (!Wait([&] {
        while (held < n && sub.Take()) ++held;  // never released
        return held == n;
      }, 10000)) {
    return 3;
  }
  Say("ready");
  Park();
}

int RunPubHold(const std::string& url) {
  Publisher pub(url);
  if (!pub.Valid()) return 4;
  if (!Wait([&] { return pub.HasSubscribers(); }, 10000)) return 3;
  auto loan = pub.Loan(64);
  if (!loan) return 4;
  Fill(*loan, 64, 1);
  pub.Publish(*loan);
  Say("ready");
  Park();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return 64;
  std::string mode = argv[1];
  std::string url = argv[2];
  auto arg = [&](int i, uint64_t def) { return argc > i ? std::strtoull(argv[i], nullptr, 10) : def; };
  if (mode == "pub") return RunPub(url, arg(3, 100), arg(4, 0) != 0, static_cast<uint32_t>(arg(5, 64)));
  if (mode == "sub") return RunSub(url, arg(3, 100), arg(4, 0) != 0, static_cast<uint32_t>(arg(5, 64)));
  if (mode == "sub_hold") return RunSubHold(url, static_cast<uint32_t>(arg(3, 1)));
  if (mode == "pub_hold") return RunPubHold(url);
  return 64;
}
