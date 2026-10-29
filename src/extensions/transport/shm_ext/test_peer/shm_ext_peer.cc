// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Peer process for the shm extension cross-process tests. It speaks the IBMW
// transport frame directly on the selfdev library, i.e. it is an independent
// encoder/decoder of the format ShmTransportDriver uses:
//   pkg_size(u32 LE) | ser_type(u8 len + bytes) | ctx_num(u8) |
//   (key u16 len + bytes | val u16 len + bytes)* | payload
// The payload is one little-endian int32.
//
//   pub <url> <count> <first>   publish values first..first+count-1 once a
//                               subscriber is connected, print "done"
//   sub <url> <count>           print "ready", receive count frames, print
//                               "got <v1> <v2> ..." (ctx "origin" must be "test")
// Exit codes: 0 ok, 2 bad frame, 3 timeout, 4 setup failure, 64 usage.

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ibmw_selfdev/shm/shm.h"

using namespace ibmw::selfdev::shm;  // NOLINT

namespace {

constexpr char kSerType[] = "test";

void Say(const std::string& s) {
  std::printf("%s\n", s.c_str());
  std::fflush(stdout);
}

template <class Pred>
bool Wait(Pred pred, int ms) {
  for (int i = 0; i < ms; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

void PutU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

std::vector<uint8_t> Encode(int32_t value) {
  std::vector<uint8_t> f(4, 0);
  f.push_back(sizeof(kSerType) - 1);
  f.insert(f.end(), kSerType, kSerType + sizeof(kSerType) - 1);
  f.push_back(1);  // one ctx pair
  const std::string key = "origin", val = "peer";
  PutU16(f, static_cast<uint16_t>(key.size()));
  f.insert(f.end(), key.begin(), key.end());
  PutU16(f, static_cast<uint16_t>(val.size()));
  f.insert(f.end(), val.begin(), val.end());
  uint8_t v[4];
  std::memcpy(v, &value, 4);
  f.insert(f.end(), v, v + 4);
  const auto size = static_cast<uint32_t>(f.size());
  std::memcpy(f.data(), &size, 4);
  return f;
}

// Returns false on any framing violation.
bool Decode(const uint8_t* p, size_t chunk_size, int32_t* value, std::string* origin) {
  if (chunk_size < 4) return false;
  uint32_t pkg;
  std::memcpy(&pkg, p, 4);
  if (pkg < 4 || pkg > chunk_size) return false;
  size_t off = 4;
  auto need = [&](size_t n) { return off + n <= pkg; };
  if (!need(1)) return false;
  const size_t ser_len = p[off++];
  if (!need(ser_len) || std::string(reinterpret_cast<const char*>(p + off), ser_len) != kSerType) return false;
  off += ser_len;
  if (!need(1)) return false;
  const size_t n = p[off++];
  origin->clear();
  for (size_t i = 0; i < n; ++i) {
    std::string kv[2];
    for (auto& s : kv) {
      if (!need(2)) return false;
      const size_t len = p[off] | (p[off + 1] << 8);
      off += 2;
      if (!need(len)) return false;
      s.assign(reinterpret_cast<const char*>(p + off), len);
      off += len;
    }
    if (kv[0] == "origin") *origin = kv[1];
  }
  if (pkg - off != 4) return false;
  std::memcpy(value, p + off, 4);
  return true;
}

int RunPub(const std::string& url, int count, int first) {
  Publisher pub(url);
  if (!pub.Valid()) return 4;
  if (!Wait([&] { return pub.HasSubscribers(); }, 10000)) return 3;
  for (int i = 0; i < count; ++i) {
    const auto frame = Encode(first + i);
    auto loan = pub.Loan(static_cast<uint32_t>(frame.size()));
    if (!loan) return 4;
    std::memcpy(*loan, frame.data(), frame.size());
    pub.Publish(*loan);
    // Latest-value delivery with queue depth 1: pace so nothing is dropped.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  Say("done");
  std::this_thread::sleep_for(std::chrono::seconds(30));
  return 0;
}

int RunSub(const std::string& url, int count) {
  Subscriber sub(url, SubscriberOptions{64, QueueFullPolicy::kDiscardOldest, 4});
  if (!sub.Valid()) return 4;
  Say("ready");
  std::string got = "got";
  for (int i = 0; i < count; ++i) {
    const void* p = nullptr;
    if (!Wait(
            [&] {
              auto t = sub.Take();
              if (!t) return false;
              p = *t;
              return true;
            },
            10000)) {
      return 3;
    }
    int32_t value = 0;
    std::string origin;
    const bool ok = Decode(static_cast<const uint8_t*>(p), sub.PayloadSize(p), &value, &origin);
    sub.Release(p);
    if (!ok || origin != "test") return 2;
    got += " " + std::to_string(value);
  }
  Say(got);
  return 0;
}

// Lossless subscriber that never takes: a stalled remote consumer.
int RunStall(const std::string& url) {
  Subscriber sub(url, SubscriberOptions{4, QueueFullPolicy::kBlockProducer, 4});
  if (!sub.Valid()) return 4;
  Say("ready");
  std::this_thread::sleep_for(std::chrono::seconds(60));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::strcmp(argv[1], "pub") == 0) return RunPub(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
  if (argc >= 4 && std::strcmp(argv[1], "sub") == 0) return RunSub(argv[2], std::atoi(argv[3]));
  if (argc >= 3 && std::strcmp(argv[1], "stall") == 0) return RunStall(argv[2]);
  std::fprintf(stderr, "usage: %s pub <url> <count> <first> | sub <url> <count> | stall <url>\n", argv[0]);
  return 64;
}
