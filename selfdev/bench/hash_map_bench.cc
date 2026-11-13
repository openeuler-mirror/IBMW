// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// ConcurrentHashMap vs tbb::concurrent_hash_map with the access pattern of
// IBMW's RpcClientTool: Emplace(req_id, recorder) when a request is sent,
// Take(req_id) (tbb: find with accessor + erase) when the response arrives.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bench_harness.h"
#include "ibmw_selfdev/concurrent/concurrent_hash_map.h"
#include "oneapi/tbb/concurrent_hash_map.h"

namespace {

namespace bench = ibmw::selfdev::bench;

// Stand-in for MsgRecorder: 64 bytes, cheap to move.
struct Value {
  uint64_t v[8];
};
static_assert(sizeof(Value) == 64);

Value MakeValue(uint32_t k) {
  Value v{};
  v.v[0] = k;
  v.v[7] = k * 7u;
  return v;
}

struct SelfdevMap {
  ibmw::selfdev::concurrent::ConcurrentHashMap<uint32_t, Value> m;
  bool Emplace(uint32_t k, const Value& v) { return m.Emplace(k, v); }
  std::optional<Value> Take(uint32_t k) { return m.Take(k); }
  bool Contains(uint32_t k) const { return m.Contains(k); }
  std::optional<Value> Get(uint32_t k) const { return m.Get(k); }
};

struct TbbMap {
  using Map = tbb::concurrent_hash_map<uint32_t, Value>;
  Map m;
  bool Emplace(uint32_t k, const Value& v) { return m.emplace(k, v); }
  // Same code as RpcClientTool::GetRecord() in the TBB branch.
  std::optional<Value> Take(uint32_t k) {
    Map::accessor ac;
    if (!m.find(ac, k)) return std::nullopt;
    auto out = std::make_optional<Value>(std::move(ac->second));
    m.erase(ac);
    return out;
  }
  bool Contains(uint32_t k) const { return m.count(k) != 0; }
  std::optional<Value> Get(uint32_t k) const {
    Map::const_accessor ac;
    if (!m.find(ac, k)) return std::nullopt;
    return ac->second;
  }
};

struct XorShift {
  uint64_t s;
  uint32_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<uint32_t>(s);
  }
};

constexpr uint32_t kPrefillBase = 0x80000000u;  // background entries never touched by workers

template <class M>
void Prefill(M& m, uint32_t live) {
  for (uint32_t i = 0; i < live; ++i) m.Emplace(kPrefillBase + i, MakeValue(i));
}

// Returns the wall time from releasing the threads until all have joined.
template <class Fn>
double RunThreads(int threads, Fn fn) {
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t) {
    ts.emplace_back([&, t] {
      ready.fetch_add(1);
      while (!go.load(std::memory_order_acquire)) {
      }
      fn(t);
    });
  }
  while (ready.load() != threads) {
  }
  const bench::Stopwatch sw;
  go.store(true, std::memory_order_release);
  for (auto& th : ts) th.join();
  return sw.ElapsedNs();
}

// Disjoint: request ids are interleaved across threads (global req_id
// counter); each thread keeps kWindow requests in flight and answers the
// oldest one after sending a new one. Returns Emplace+Take pairs.
template <class M>
bench::Sample EmplaceTakeDisjoint(int threads, uint32_t live, uint64_t pairs_per_thread) {
  constexpr uint64_t kWindow = 8;
  M m;
  Prefill(m, live);
  const double ns = RunThreads(threads, [&](int t) {
    auto key = [&](uint64_t i) { return static_cast<uint32_t>(i * static_cast<uint64_t>(threads) + static_cast<uint64_t>(t)); };
    uint64_t sink = 0;
    for (uint64_t i = 0; i < pairs_per_thread + kWindow; ++i) {
      if (i < pairs_per_thread) m.Emplace(key(i), MakeValue(key(i)));
      if (i >= kWindow) {
        auto v = m.Take(key(i - kWindow));
        sink += v ? v->v[0] : 0;
      }
    }
    bench::DoNotOptimize(sink);
  });
  return {pairs_per_thread * static_cast<uint64_t>(threads), ns};
}

// Overlapping: every thread draws keys from the same 1024-key range, so
// Emplace/Take hit the same buckets/shards and sometimes fail.
template <class M>
bench::Sample EmplaceTakeOverlap(int threads, uint32_t live, uint64_t pairs_per_thread) {
  M m;
  Prefill(m, live);
  const double ns = RunThreads(threads, [&](int t) {
    XorShift rng{0x9e3779b97f4a7c15ULL * static_cast<uint64_t>(t + 1)};
    uint64_t sink = 0;
    for (uint64_t i = 0; i < pairs_per_thread; ++i) {
      const uint32_t k = rng.Next() & 1023u;
      m.Emplace(k, MakeValue(k));
      auto v = m.Take(rng.Next() & 1023u);
      sink += v ? v->v[0] : 0;
    }
    bench::DoNotOptimize(sink);
  });
  return {pairs_per_thread * static_cast<uint64_t>(threads), ns};
}

// Read-mostly: 45% Contains, 45% Get on prefilled keys, 10% Emplace+Take of a
// thread-private key. Returns operations (an Emplace+Take pair counts as 1).
template <class M>
bench::Sample ReadMostly(int threads, uint32_t live, uint64_t ops_per_thread) {
  M m;
  Prefill(m, live);
  const double ns = RunThreads(threads, [&](int t) {
    XorShift rng{0xd1b54a32d192ed03ULL * static_cast<uint64_t>(t + 1)};
    uint64_t sink = 0;
    uint32_t own = static_cast<uint32_t>(t) << 20;
    for (uint64_t i = 0; i < ops_per_thread; ++i) {
      const uint32_t r = rng.Next();
      const uint32_t pick = r % 100u;
      const uint32_t k = kPrefillBase + (r >> 8) % live;
      if (pick < 45) {
        sink += m.Contains(k) ? 1 : 0;
      } else if (pick < 90) {
        auto v = m.Get(k);
        sink += v ? v->v[7] : 0;
      } else {
        m.Emplace(own, MakeValue(own));
        auto v = m.Take(own);
        sink += v ? v->v[0] : 0;
        ++own;
      }
    }
    bench::DoNotOptimize(sink);
  });
  return {ops_per_thread * static_cast<uint64_t>(threads), ns};
}

}  // namespace

int main(int argc, char** argv) {
  bench::Runner r(bench::ParseOptions(argc, argv));
  for (uint32_t live : {100u, 100'000u}) {
    const std::string sz = live == 100 ? "live100" : "live100k";
    for (int t : {1, 4, 8, 16}) {
      const std::string th = std::to_string(t) + "T";
      const uint64_t per = r.N(2'000'000) / static_cast<uint64_t>(t);
      r.Compare("map/emplace+take/disjoint/" + sz + "/" + th,
                [&] { return EmplaceTakeDisjoint<SelfdevMap>(t, live, per); },
                [&] { return EmplaceTakeDisjoint<TbbMap>(t, live, per); }, "tbb");
      r.Compare("map/emplace+take/overlap1024/" + sz + "/" + th,
                [&] { return EmplaceTakeOverlap<SelfdevMap>(t, live, per); },
                [&] { return EmplaceTakeOverlap<TbbMap>(t, live, per); }, "tbb");
      r.Compare("map/read-mostly 90%/" + sz + "/" + th, [&] { return ReadMostly<SelfdevMap>(t, live, per * 2); },
                [&] { return ReadMostly<TbbMap>(t, live, per * 2); }, "tbb");
    }
  }
  return 0;
}
