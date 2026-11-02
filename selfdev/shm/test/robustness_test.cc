// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Robustness: hostile peers speaking the raw protocol, read-only payloads,
// stale sockets, crashed processes (kill -9), cross-process data paths and
// domain isolation.

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
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

using namespace std::chrono_literals;
using namespace detail;  // NOLINT
using test::CheckPattern;
using test::FillPattern;
using test::UniqueUrl;
using test::WaitFor;

std::string HashPrefix(const std::string& url) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016" PRIx64, Hash64(url));
  return buf;
}

std::vector<std::string> SocketsFor(const std::string& url) {
  std::vector<std::string> out;
  std::string prefix = HashPrefix(url) + ".";
  if (DIR* d = ::opendir(Runtime::ServiceDirectory().c_str())) {
    while (auto* de = ::readdir(d)) {
      std::string n = de->d_name;
      if (n.rfind(prefix, 0) == 0 && n.size() > 5 && n.substr(n.size() - 5) == ".sock") {
        out.push_back(Runtime::ServiceDirectory() + "/" + n);
      }
    }
    ::closedir(d);
  }
  return out;
}

void PublishSeq(Publisher& pub, uint64_t seq, uint32_t size = 64) {
  auto l = pub.Loan(size);
  ASSERT_TRUE(l.ok()) << ErrorName(l.error());
  FillPattern(*l, size, seq);
  pub.Publish(*l);
}

// A subscriber implemented directly on the wire protocol, so tests can
// misbehave in ways the real Subscriber never would.
class RawSubscriber {
 public:
  bool Connect(const std::string& url) {
    auto paths = SocketsFor(url);
    if (paths.size() != 1) return false;
    sock_ = UniqueFd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, paths[0].c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(sock_.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    auto n = CreateSegment("raw_notify", kNotifySegmentSize, false, true);
    if (!n) return false;
    notify_ = std::move(n.value());
    reinterpret_cast<NotifySegment*>(notify_.map.Data())->magic = kNotifyMagic;
    return true;
  }

  bool SendHello(const std::string& url, uint32_t abi = kAbiVersion, uint32_t cap = 4, uint32_t held = 4) {
    HelloMsg h{};
    h.h = {kMsgMagic, MsgType::kHello};
    h.abi = abi;
    h.queue_capacity = cap;
    h.queue_full = 0;
    h.max_held = held;
    h.url_length = static_cast<uint32_t>(url.size());
    std::memcpy(h.url, url.data(), url.size());
    int fd = notify_.fd.Get();
    return SendMessage(sock_.Get(), &h, offsetof(HelloMsg, url) + url.size(), &fd, 1);
  }

  bool SendRaw(const void* data, size_t len) { return SendMessage(sock_.Get(), data, len, nullptr, 0); }

  // Next message; returns its length, 0 on close, -1 on timeout.
  ssize_t Receive(std::vector<uint8_t>* buf, std::vector<UniqueFd>* fds, std::chrono::milliseconds timeout = 3s) {
    struct pollfd pfd {sock_.Get(), POLLIN, 0};
    if (::poll(&pfd, 1, static_cast<int>(timeout.count())) <= 0) return -1;
    buf->assign(kMaxMsgSize, 0);
    fds->clear();
    ssize_t n = ReceiveMessage(sock_.Get(), buf->data(), buf->size(), fds);
    return n < 0 ? 0 : n;
  }

  // Full handshake: Welcome + SegmentAdds, map everything, ack.
  bool Handshake(const std::string& url, uint32_t expected_segments) {
    if (!SendHello(url)) return false;
    std::vector<uint8_t> buf;
    std::vector<UniqueFd> fds;
    if (Receive(&buf, &fds) <= 0) return false;
    WelcomeMsg wm{};
    std::memcpy(&wm, buf.data(), sizeof(wm));
    if (wm.h.type != MsgType::kWelcome || fds.size() != 1) return false;
    auto m = MapSegment(fds[0].Get(), wm.segment_size, true);
    if (!m) return false;
    conn_ = std::move(m.value());
    hdr = reinterpret_cast<ConnHeader*>(conn_.Data());
    dslots = wm.delivery_slots;
    rslots = wm.return_slots;
    MsgHeader ack{kMsgMagic, MsgType::kConnAck};
    if (!SendRaw(&ack, sizeof(ack))) return false;
    for (uint32_t i = 0; i < expected_segments; ++i) {
      if (Receive(&buf, &fds) <= 0) return false;
      SegmentAddMsg sm{};
      std::memcpy(&sm, buf.data(), sizeof(sm));
      if (sm.h.type != MsgType::kSegmentAdd || fds.size() != 1) return false;
      writable_data_map_refused = writable_data_map_refused && !MapSegment(fds[0].Get(), sm.segment_size, true).ok();
      auto dm = MapSegment(fds[0].Get(), sm.segment_size, false);
      if (!dm) return false;
      data_.push_back({std::move(dm.value()), sm.chunk_stride});
      SegmentAckMsg sa{{kMsgMagic, MsgType::kSegmentAck}, sm.segment_id};
      if (!SendRaw(&sa, sizeof(sa))) return false;
    }
    return true;
  }

  // Takes one handle off the delivery ring (as Subscriber::Take does).
  bool TakeHandle(uint64_t* handle) {
    uint64_t r = hdr->delivery_read.load();
    if (r >= hdr->delivery_write.load()) return false;
    *handle = DeliverySlots(hdr)[r % dslots].load();
    return hdr->delivery_read.compare_exchange_strong(r, r + 1);
  }

  void Return(uint64_t handle) {
    uint64_t w = hdr->return_write.load();
    ReturnSlots(hdr, dslots)[w % rslots].store(handle);
    hdr->return_write.store(w + 1);
  }

  bool Closed() {
    std::vector<uint8_t> buf;
    std::vector<UniqueFd> fds;
    for (;;) {
      ssize_t n = Receive(&buf, &fds);
      if (n == 0) return true;
      if (n < 0) return false;
    }
  }

  // Payload of a delivered handle, through our read-only data mapping.
  const void* Payload(uint64_t handle) const {
    const auto& d = data_.at(HandleSegment(handle));
    return d.map.Data() + HandleIndex(handle) * d.stride + kChunkHeaderSize;
  }

  // Drops the control socket without an orderly close (no `closed` flag):
  // what a broken connection looks like to the publisher. Mappings stay.
  void DropSocket() { sock_.Reset(); }

  ConnHeader* hdr = nullptr;
  uint32_t dslots = 0, rslots = 0;
  bool writable_data_map_refused = true;

 private:
  struct DataMap {
    Mapping map;
    uint64_t stride;
  };
  UniqueFd sock_;
  Segment notify_;
  Mapping conn_;
  std::vector<DataMap> data_;
};

PublisherOptions EagerPool() {
  PublisherOptions po;
  po.pools = {{64, 16}};
  return po;
}

// RB-01 ABI mismatch is rejected with a reason, the publisher keeps serving.
TEST(ShmRobustnessTest, RejectsAbiMismatch) {
  auto url = UniqueUrl("rb01");
  Publisher pub(url);
  RawSubscriber raw;
  ASSERT_TRUE(raw.Connect(url));
  ASSERT_TRUE(raw.SendHello(url, kAbiVersion + 1));
  std::vector<uint8_t> buf;
  std::vector<UniqueFd> fds;
  ASSERT_GT(raw.Receive(&buf, &fds), 0);
  RejectMsg rm{};
  std::memcpy(&rm, buf.data(), sizeof(rm));
  EXPECT_EQ(rm.h.type, MsgType::kReject);
  EXPECT_EQ(rm.reason, static_cast<uint32_t>(Error::kAbiMismatch));
  EXPECT_TRUE(raw.Closed());

  Subscriber sub(url);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 1; }));
}

// RB-02 Wrong URL / missing notify fd / garbage messages close the connection.
TEST(ShmRobustnessTest, RejectsMalformedHandshake) {
  auto url = UniqueUrl("rb02");
  Publisher pub(url);
  {
    RawSubscriber raw;
    ASSERT_TRUE(raw.Connect(url));
    ASSERT_TRUE(raw.SendHello(url + "x"));
    std::vector<uint8_t> buf;
    std::vector<UniqueFd> fds;
    ASSERT_GT(raw.Receive(&buf, &fds), 0);
    RejectMsg rm{};
    std::memcpy(&rm, buf.data(), sizeof(rm));
    EXPECT_EQ(rm.reason, static_cast<uint32_t>(Error::kInvalidArg));
    EXPECT_TRUE(raw.Closed());
  }
  {
    RawSubscriber raw;
    ASSERT_TRUE(raw.Connect(url));
    uint32_t junk[4] = {0xdeadbeef, 1, 2, 3};
    ASSERT_TRUE(raw.SendRaw(junk, sizeof(junk)));
    EXPECT_TRUE(raw.Closed());
  }
  {
    RawSubscriber raw;
    ASSERT_TRUE(raw.Connect(url));
    MsgHeader early_ack{kMsgMagic, MsgType::kConnAck};  // before Hello
    ASSERT_TRUE(raw.SendRaw(&early_ack, sizeof(early_ack)));
    EXPECT_TRUE(raw.Closed());
  }
  EXPECT_EQ(pub.Stats().connections, 0u);
}

// RB-03 Bad and duplicate returns are counted and ignored; the genuine
// return is honoured exactly once.
TEST(ShmRobustnessTest, BadReturnsAreIgnored) {
  auto url = UniqueUrl("rb03");
  Publisher pub(url, EagerPool());
  RawSubscriber raw;
  ASSERT_TRUE(raw.Connect(url));
  ASSERT_TRUE(raw.Handshake(url, 1));
  EXPECT_TRUE(raw.writable_data_map_refused);
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  uint64_t h = 0;
  ASSERT_TRUE(WaitFor([&] { return raw.TakeHandle(&h); }));
  raw.Return(h);
  raw.Return(h);                                         // duplicate
  raw.Return(MakeHandle(4095, 0, 0));                    // no such segment
  raw.Return(MakeHandle(0, 15, 0));                      // never delivered
  raw.Return(MakeHandle(HandleSegment(h), HandleIndex(h), HandleGeneration(h) + 1));  // stale generation
  PublishSeq(pub, 2);  // Publish drains return rings first
  auto st = pub.Stats();
  EXPECT_EQ(st.bad_returns, 4u);
  EXPECT_EQ(st.corrupted_connections, 0u);
  EXPECT_EQ(st.connections, 1u);
  // The pool is intact: 16 chunks still cycle without growth.
  for (uint64_t i = 3; i < 100; ++i) {
    PublishSeq(pub, i);
    while (raw.TakeHandle(&h)) raw.Return(h);
  }
  EXPECT_EQ(pub.Stats().segments, 1u);
}

// RB-04 A read index outside [write - capacity, write] marks the
// connection corrupt: the subscriber is dropped and its chunks reclaimed.
TEST(ShmRobustnessTest, CorruptReadIndexDropsSubscriber) {
  auto url = UniqueUrl("rb04");
  Publisher pub(url, EagerPool());
  Subscriber good(url);
  RawSubscriber raw;
  ASSERT_TRUE(raw.Connect(url));
  ASSERT_TRUE(raw.Handshake(url, 1));
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 2; }));
  PublishSeq(pub, 1);
  // Wait until the sample left the segment-ack queue and sits in the ring.
  ASSERT_TRUE(WaitFor([&] { return raw.hdr->delivery_write.load() == 1; }));
  raw.hdr->delivery_read.store(raw.hdr->delivery_write.load() + 100);
  PublishSeq(pub, 2);
  auto st = pub.Stats();
  EXPECT_EQ(st.corrupted_connections, 1u);
  EXPECT_TRUE(raw.Closed());
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 1; }));
  // The well-behaved subscriber is unaffected.
  PublishSeq(pub, 3);
  ASSERT_TRUE(WaitFor([&] { return good.HasData(); }));
}

// RB-05 A return write index running ahead of the ring is corruption too.
TEST(ShmRobustnessTest, CorruptReturnIndexDropsSubscriber) {
  auto url = UniqueUrl("rb05");
  Publisher pub(url, EagerPool());
  RawSubscriber raw;
  ASSERT_TRUE(raw.Connect(url));
  ASSERT_TRUE(raw.Handshake(url, 1));
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return raw.hdr->delivery_write.load() == 1; }));
  raw.hdr->return_write.store(raw.hdr->return_read.load() + 1000);
  PublishSeq(pub, 2);
  EXPECT_EQ(pub.Stats().corrupted_connections, 1u);
  EXPECT_TRUE(raw.Closed());
  // Both published chunks were reclaimed: the pool did not grow.
  for (uint64_t i = 3; i < 50; ++i) PublishSeq(pub, i);
  EXPECT_EQ(pub.Stats().segments, 1u);
}

// RB-06 Subscribers cannot write into received payloads (read-only mapping).
TEST(ShmRobustnessDeathTest, SubscriberPayloadIsReadOnly) {
  auto url = UniqueUrl("rb06");
  Publisher pub(url);
  // The kernel's SIGSEGV must kill the peer, not a sanitizer's handler.
  auto peer = test::SpawnPeer({"sub_write", url},
                              {"ASAN_OPTIONS=handle_segv=0", "TSAN_OPTIONS=handle_segv=0", "UBSAN_OPTIONS=handle_segv=0"});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  auto l = pub.Loan(64);
  ASSERT_TRUE(l.ok());
  pub.Publish(*l);
  EXPECT_EQ(test::WaitPeer(peer), 128 + SIGSEGV);
}

// RB-07 A socket file left by a dead publisher (ECONNREFUSED) is removed by
// the next subscriber; a live one is not.
TEST(ShmRobustnessTest, StaleSocketCleanup) {
  auto url = UniqueUrl("rb07");
  std::string stale = Runtime::ServiceDirectory() + "/" + HashPrefix(url) + ".999999.deadbeef.sock";
  {
    UniqueFd s(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, stale.c_str(), sizeof(addr.sun_path) - 1);
    ASSERT_EQ(::bind(s.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  }  // bound, never listened, closed: connect() gets ECONNREFUSED
  Publisher live(url);
  ASSERT_EQ(SocketsFor(url).size(), 2u);
  Subscriber sub(url);
  ASSERT_TRUE(WaitFor([&] { return SocketsFor(url).size() == 1; }));
  EXPECT_EQ(::access(stale.c_str(), F_OK), -1);
  ASSERT_TRUE(WaitFor([&] { return live.HasSubscribers(); }));
}

// RB-08 Cross-process, test publishes: the child receives every sample of a
// lossless stream, in order, and verifies the bytes.
TEST(ShmRobustnessTest, CrossProcessToChild) {
  auto url = UniqueUrl("rb08");
  PublisherOptions po;
  po.too_slow = TooSlowPolicy::kWaitForConsumer;
  Publisher pub(url, po);
  auto peer = test::SpawnPeer({"sub", url, "5000", "1", "4096"});
  ASSERT_EQ(test::ReadLine(peer), "ready");
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }, 10s));
  for (uint64_t s = 1; s <= 5000; ++s) PublishSeq(pub, s, 4096);
  EXPECT_EQ(test::ReadLine(peer), "done 5000");
  EXPECT_EQ(test::WaitPeer(peer), 0);
  EXPECT_EQ(pub.Stats().evictions, 0u);
}

// RB-09 Cross-process, child publishes; the listener is woken through the
// shared futex in our notify segment.
TEST(ShmRobustnessTest, CrossProcessFromChildWithListener) {
  auto url = UniqueUrl("rb09");
  Subscriber sub(url, SubscriberOptions{8, QueueFullPolicy::kBlockProducer, 8});
  struct Ctx {
    std::atomic<uint64_t> got{0}, last{0}, errors{0};
  } ctx;
  Listener listener;
  ASSERT_TRUE(listener
                  .Attach(sub,
                          [](Subscriber* s, void* p) {
                            auto* c = static_cast<Ctx*>(p);
                            while (auto t = s->Take()) {
                              uint64_t seq = CheckPattern(*t, 1024);
                              if (seq != c->last.load() + 1) c->errors.fetch_add(1);
                              c->last = seq;
                              c->got.fetch_add(1);
                              s->Release(*t);
                            }
                          },
                          &ctx)
                  .ok());
  auto peer = test::SpawnPeer({"pub", url, "20000", "1", "1024"});
  ASSERT_EQ(test::ReadLine(peer), "ready");
  EXPECT_EQ(test::ReadLine(peer, 30s), "done 20000");
  EXPECT_TRUE(WaitFor([&] { return ctx.got.load() == 20000; }, 10s)) << ctx.got.load();
  EXPECT_EQ(ctx.errors.load(), 0u);
  test::KillPeer(peer, SIGTERM);
  EXPECT_EQ(test::WaitPeer(peer), 128 + SIGTERM);
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 0; }));
}

// RB-10 kill -9 of a subscriber holding chunks: the publisher notices the
// hang-up and reclaims everything it had delivered to it.
TEST(ShmRobustnessTest, SubscriberCrashReclaimsChunks) {
  auto url = UniqueUrl("rb10");
  Publisher pub(url, EagerPool());
  auto peer = test::SpawnPeer({"sub_hold", url, "5"});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }, 10s));
  for (uint64_t s = 1; s <= 10; ++s) PublishSeq(pub, s);  // 5 held + 5 queued
  ASSERT_EQ(test::ReadLine(peer), "ready");
  test::KillPeer(peer, SIGKILL);
  EXPECT_EQ(test::WaitPeer(peer), 128 + SIGKILL);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 0; }));
  auto st = pub.Stats();
  EXPECT_EQ(st.reclaimed, 10u);
  // All 16 eager chunks are free again.
  std::vector<void*> loans;
  for (int round = 0; round < 4; ++round) {
    for (uint32_t i = 0; i < kMaxChunksAllocatedPerPublisher; ++i) {
      auto l = pub.Loan(64);
      ASSERT_TRUE(l.ok());
      loans.push_back(*l);
    }
    for (auto* l : loans) pub.Publish(l);
    loans.clear();
  }
  EXPECT_EQ(pub.Stats().segments, 1u);
}

// RB-11 kill -9 of a publisher: the subscriber drops the connection, keeps
// the chunk it holds readable, and the dead socket file is cleaned up by
// the next subscriber.
TEST(ShmRobustnessTest, PublisherCrash) {
  auto url = UniqueUrl("rb11");
  Subscriber sub(url);
  auto peer = test::SpawnPeer({"pub_hold", url});
  ASSERT_EQ(test::ReadLine(peer), "ready");
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto t = sub.Take();
  ASSERT_TRUE(t.ok());
  test::KillPeer(peer, SIGKILL);
  EXPECT_EQ(test::WaitPeer(peer), 128 + SIGKILL);
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 0; }));
  EXPECT_EQ(CheckPattern(*t, 64), 1u);
  sub.Release(*t);
  EXPECT_EQ(SocketsFor(url).size(), 1u);  // left behind by SIGKILL
  Subscriber next(url);
  ASSERT_TRUE(WaitFor([&] { return SocketsFor(url).empty(); }));
}

// RB-12 Different IBMW_SHM_DOMAIN values never see each other.
TEST(ShmRobustnessTest, DomainIsolation) {
  auto url = UniqueUrl("rb12");
  Subscriber sub(url);
  auto peer = test::SpawnPeer({"pub_hold", url}, {"IBMW_SHM_DOMAIN=other"});
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(sub.Stats().connections, 0u);
  test::KillPeer(peer, SIGKILL);
  test::WaitPeer(peer);
}

// RB-13 (third review, H1) A control message of unknown type from a
// compatible subscriber (a newer version) is ignored, fds and all; the
// connection keeps working. Before: the publisher dropped the subscriber.
TEST(ShmRobustnessTest, UnknownSubscriberMessageIsIgnored) {
  auto url = UniqueUrl("rb13");
  Publisher pub(url, EagerPool());
  RawSubscriber raw;
  ASSERT_TRUE(raw.Connect(url));
  ASSERT_TRUE(raw.Handshake(url, 1));
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  MsgHeader future{kMsgMagic, static_cast<MsgType>(77)};
  ASSERT_TRUE(raw.SendRaw(&future, sizeof(future)));
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().unknown_messages == 1; }));
  PublishSeq(pub, 1);
  uint64_t h = 0;
  ASSERT_TRUE(WaitFor([&] { return raw.TakeHandle(&h); }));
  EXPECT_EQ(CheckPattern(raw.Payload(h), 64), 1u);
  auto st = pub.Stats();
  EXPECT_EQ(st.connections, 1u);
  EXPECT_EQ(st.corrupted_connections, 0u);
}

// RB-14 (third review, H1) A subscriber whose control socket breaks without
// an orderly close may still read what it holds: the publisher quarantines
// those chunks instead of reusing them. The subscriber is in this process,
// so its death cannot be checked (unverifiable_peers); a late orderly close
// (the `closed` flag) releases them. Before: the chunk was reclaimed at the
// hang-up and overwritten by the next loans.
TEST(ShmRobustnessTest, BrokenConnectionQuarantinesHeldChunks) {
  auto url = UniqueUrl("rb14");
  PublisherOptions po;
  po.pools = {{64, kMaxChunksAllocatedPerPublisher}};
  Publisher pub(url, po);
  RawSubscriber raw;
  ASSERT_TRUE(raw.Connect(url));
  ASSERT_TRUE(raw.Handshake(url, 1));
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  uint64_t h = 0;
  ASSERT_TRUE(WaitFor([&] { return raw.TakeHandle(&h); }));
  const void* held = raw.Payload(h);
  raw.DropSocket();
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 0; }));
  auto st = pub.Stats();
  EXPECT_EQ(st.quarantined, 1u);
  EXPECT_EQ(st.quarantined_chunks, 1u);
  EXPECT_EQ(st.unverifiable_peers, 1u);
  EXPECT_EQ(st.reclaimed, 0u);
  // Every other chunk of the pool is loaned and written many times over;
  // the quarantined one is never among them.
  for (uint64_t seq = 100; seq < 400; ++seq) {
    std::vector<void*> loans;
    for (uint32_t i = 0; i + 1 < kMaxChunksAllocatedPerPublisher; ++i) {
      auto l = pub.Loan(64);
      ASSERT_TRUE(l.ok()) << ErrorName(l.error());
      FillPattern(*l, 64, seq);
      loans.push_back(*l);
    }
    for (auto* l : loans) pub.Publish(l);
  }
  EXPECT_EQ(CheckPattern(held, 64), 1u);
  EXPECT_EQ(pub.Stats().segments, 1u);
  // The last chunk is the quarantined one: the pool has to grow for it.
  {
    std::vector<void*> loans;
    for (uint32_t i = 0; i < kMaxChunksAllocatedPerPublisher; ++i) {
      auto l = pub.Loan(64);
      ASSERT_TRUE(l.ok());
      loans.push_back(*l);
    }
    EXPECT_EQ(pub.Stats().segments, 2u);
    for (auto* l : loans) pub.Release(l);
  }
  EXPECT_EQ(CheckPattern(held, 64), 1u);
  // Done reading: the late orderly close gives the chunk back.
  raw.hdr->closed.store(1, std::memory_order_release);
  st = pub.Stats();
  EXPECT_EQ(st.quarantined_chunks, 0u);
  EXPECT_EQ(st.reclaimed, 1u);
}

// RB-15 (third review, H1) An orderly close still reclaims at once: a
// Subscriber destroyed while holding samples, and a raw subscriber that
// sets `closed` before its socket goes down.
TEST(ShmRobustnessTest, OrderlyCloseReclaimsAtOnce) {
  auto url = UniqueUrl("rb15");
  Publisher pub(url, EagerPool());
  {
    Subscriber sub(url);
    ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
    PublishSeq(pub, 1);
    ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
    ASSERT_TRUE(sub.Take().ok());  // held at destruction
  }
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 0; }));
  auto st = pub.Stats();
  EXPECT_EQ(st.reclaimed, 1u);
  EXPECT_EQ(st.quarantined, 0u);
  RawSubscriber raw;
  ASSERT_TRUE(raw.Connect(url));
  ASSERT_TRUE(raw.Handshake(url, 1));
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 2);
  uint64_t h = 0;
  ASSERT_TRUE(WaitFor([&] { return raw.TakeHandle(&h); }));
  raw.hdr->closed.store(1, std::memory_order_release);
  raw.DropSocket();
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 0; }));
  st = pub.Stats();
  EXPECT_EQ(st.reclaimed, 2u);
  EXPECT_EQ(st.quarantined, 0u);
  EXPECT_EQ(st.quarantined_chunks, 0u);
}

// Shared by RB-16/RB-17: a live subscriber process holding 5 chunks loses
// its control sockets. Its chunks are quarantined while it lives and given
// back once it is dead (and reaped).
void QuarantineUntilPeerDies(const std::string& url) {
  PublisherOptions po;
  po.pools = {{64, kMaxChunksAllocatedPerPublisher}};
  Publisher pub(url, po);
  auto peer = test::SpawnPeer({"sub_hold_drop", url, "5"});
  struct Reap {  // a failed assertion must not leave the child parked
    test::Peer& p;
    ~Reap() {
      if (p.pid > 0) {
        test::KillPeer(p, SIGKILL);
        test::WaitPeer(p);
      }
    }
  } reap{peer};
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }, 10s));
  std::vector<void*> held;  // the child takes the first 5 samples
  for (uint64_t s = 1; s <= 5; ++s) {
    auto l = pub.Loan(64);
    ASSERT_TRUE(l.ok());
    FillPattern(*l, 64, s);
    held.push_back(*l);
    pub.Publish(*l);
  }
  ASSERT_EQ(test::ReadLine(peer), "ready");
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 0; }));
  auto st = pub.Stats();
  EXPECT_EQ(st.quarantined_chunks, 5u);
  EXPECT_EQ(st.unverifiable_peers, 0u);
  // While it lives none of its chunks comes back, however often we loan.
  for (int round = 0; round < 50; ++round) {
    std::vector<void*> loans;
    for (uint32_t i = 0; i + 5 < kMaxChunksAllocatedPerPublisher; ++i) {
      auto l = pub.Loan(64);
      ASSERT_TRUE(l.ok());
      EXPECT_EQ(std::count(held.begin(), held.end(), *l), 0);
      loans.push_back(*l);
    }
    for (auto* l : loans) pub.Release(l);
  }
  std::this_thread::sleep_for(100ms);  // pidfd watch / polling timer had time to misfire
  EXPECT_EQ(pub.Stats().quarantined_chunks, 5u);
  test::KillPeer(peer, SIGKILL);
  EXPECT_EQ(test::WaitPeer(peer), 128 + SIGKILL);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().quarantined_chunks == 0; }));
  EXPECT_EQ(pub.Stats().reclaimed, 5u);
  // All chunks are back: no growth.
  std::vector<void*> loans;
  for (uint32_t i = 0; i < kMaxChunksAllocatedPerPublisher; ++i) {
    auto l = pub.Loan(64);
    ASSERT_TRUE(l.ok());
    loans.push_back(*l);
  }
  for (auto* l : loans) pub.Release(l);
  EXPECT_EQ(pub.Stats().segments, 1u);
}

// RB-16 (third review, H1) ... death seen through pidfd.
TEST(ShmRobustnessTest, QuarantineEndsWithPeerProcess) {
  QuarantineUntilPeerDies(UniqueUrl("rb16"));
}

// RB-17 (third review, H1) ... without pidfd (old kernel, sandbox): death
// seen by kill(pid, 0) polling.
TEST(ShmRobustnessTest, QuarantineEndsWithPeerProcessWithoutPidfd) {
  struct NoPidfd {
    NoPidfd() { fault::no_pidfd.store(true); }
    ~NoPidfd() { fault::no_pidfd.store(false); }
  } no_pidfd;
  QuarantineUntilPeerDies(UniqueUrl("rb17"));
}

// A subscriber process holding 3 chunks that lost its control sockets; the
// peer stays parked until the fixture ends.
class QuarantinedPeer {
 public:
  QuarantinedPeer(const std::string& url, const char* mode, const PublisherOptions& po) : pub(url, po) {
    peer = test::SpawnPeer({mode, url, "3"});
    EXPECT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }, 10s));
    for (uint64_t s = 1; s <= 3; ++s) PublishSeq(pub, s);
    EXPECT_EQ(test::ReadLine(peer), "ready");
    EXPECT_TRUE(WaitFor([&] { return pub.Stats().connections == 0; }));
    EXPECT_EQ(pub.Stats().quarantined_chunks, 3u);
  }
  ~QuarantinedPeer() { Kill(); }
  void Kill() {
    if (peer.pid > 0) {
      test::KillPeer(peer, SIGKILL);
      test::WaitPeer(peer);
    }
  }

  Publisher pub;
  test::Peer peer;
};

int CountPidfds() {
  int n = 0;
  if (DIR* d = ::opendir("/proc/self/fd")) {
    while (auto* de = ::readdir(d)) {
      char target[64];
      ssize_t len = ::readlink((std::string("/proc/self/fd/") + de->d_name).c_str(), target, sizeof(target) - 1);
      if (len > 0) {
        target[len] = '\0';
        n += std::strcmp(target, "anon_inode:[pidfd]") == 0 ? 1 : 0;
      }
    }
    ::closedir(d);
  }
  return n;
}

// RB-18 (fourth review) A subscriber that stopped reading its control socket
// hits the send queue bound end to end: it is dropped, what it holds stays
// quarantined and comes back with its orderly close. A compliant peer cannot
// reach the real bound, so a hook lowers it.
TEST(ShmRobustnessTest, SendQueueOverflowQuarantines) {
  struct Restore {
    ~Restore() {
      fault::send_would_block.store(0);
      fault::send_queue_limit.store(0);
    }
  } restore;
  auto url = UniqueUrl("rb18");
  Publisher pub(url, EagerPool());
  RawSubscriber raw;
  ASSERT_TRUE(raw.Connect(url));
  ASSERT_TRUE(raw.Handshake(url, 1));
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  uint64_t held = 0;
  ASSERT_TRUE(WaitFor([&] { return raw.TakeHandle(&held); }));

  fault::send_queue_limit.store(1);
  fault::send_would_block.store(1000);
  for (uint32_t size : {1024u, 4096u}) {  // new size classes: one SegmentAdd each
    auto l = pub.Loan(size);
    ASSERT_TRUE(l.ok()) << ErrorName(l.error());
    pub.Release(*l);
  }
  fault::send_would_block.store(0);
  fault::send_queue_limit.store(0);

  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 0; }));
  auto st = pub.Stats();
  EXPECT_EQ(st.send_queue_overflows, 1u);
  EXPECT_EQ(st.quarantined_chunks, 1u);
  EXPECT_EQ(st.reclaimed, 0u);
  for (uint64_t s = 2; s < 100; ++s) {
    auto l = pub.Loan(64);
    ASSERT_TRUE(l.ok()) << ErrorName(l.error());
    FillPattern(*l, 64, s);
    pub.Release(*l);
  }
  EXPECT_EQ(CheckPattern(raw.Payload(held), 64), 1u);
  EXPECT_TRUE(raw.Closed());
  raw.hdr->closed.store(1, std::memory_order_release);
  EXPECT_EQ(pub.Stats().quarantined_chunks, 0u);
  EXPECT_EQ(pub.Stats().reclaimed, 1u);
}

// RB-19 (fourth review) A late orderly close while a pidfd watches the process:
// the process lives on, so the watch would never fire. Reaping asks the
// control thread to drop it and the pidfd goes (Reap -> Post -> PeerExited).
TEST(ShmRobustnessTest, LateOrderlyCloseDropsPidfdWatch) {
  auto url = UniqueUrl("rb19");
  const int before = CountPidfds();
  QuarantinedPeer q(url, "sub_hold_drop_release", {});
  const bool have_pidfd = CountPidfds() == before + 1;
  RecordProperty("pidfd_watch", have_pidfd ? "yes" : "no");
  test::KillPeer(q.peer, SIGUSR1);
  ASSERT_EQ(test::ReadLine(q.peer), "released");
  ASSERT_TRUE(WaitFor([&] { return q.pub.Stats().quarantined_chunks == 0; }));
  EXPECT_EQ(q.pub.Stats().reclaimed, 3u);
  EXPECT_TRUE(WaitFor([&] { return CountPidfds() == before; }));
  EXPECT_EQ(::kill(q.peer.pid, 0), 0);  // still running, no death involved
}

// RB-20 (fourth review, L-8) kill(pid, 0) refused: nothing can be learned about
// the process, and never will be. That is not "gone": the chunks stay and
// polling ends, however the process fares later.
TEST(ShmRobustnessTest, RefusedKillStopsPollingAndKeepsChunks) {
  struct Faults {
    Faults() {
      fault::no_pidfd.store(true);
      fault::kill_eperm.store(true);
    }
    ~Faults() {
      fault::no_pidfd.store(false);
      fault::kill_eperm.store(false);
    }
  } faults;
  QuarantinedPeer q(UniqueUrl("rb20"), "sub_hold_drop", {});
  EXPECT_EQ(q.pub.Stats().unverifiable_peers, 1u);
  q.Kill();
  fault::kill_eperm.store(false);  // a poll now would see ESRCH
  std::this_thread::sleep_for(200ms);
  auto st = q.pub.Stats();
  EXPECT_EQ(st.quarantined_chunks, 3u);
  EXPECT_EQ(st.reclaimed, 0u);
  EXPECT_EQ(st.unverifiable_peers, 1u);
}

// RB-21 (fourth review) PublisherOptions::peer_death_check off: the death of the
// process releases nothing, only an orderly close or the end of the publisher.
TEST(ShmRobustnessTest, PeerDeathCheckCanBeDisabled) {
  PublisherOptions po;
  po.peer_death_check = false;
  const int before = CountPidfds();
  QuarantinedPeer q(UniqueUrl("rb21"), "sub_hold_drop", po);
  EXPECT_EQ(CountPidfds(), before);  // no process is looked at
  EXPECT_EQ(q.pub.Stats().unverifiable_peers, 1u);
  q.Kill();
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(q.pub.Stats().quarantined_chunks, 3u);
}

}  // namespace
}  // namespace ibmw::selfdev::shm
