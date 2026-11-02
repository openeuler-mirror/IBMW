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

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

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

  ConnHeader* hdr = nullptr;
  uint32_t dslots = 0, rslots = 0;
  bool writable_data_map_refused = true;

 private:
  UniqueFd sock_;
  Segment notify_;
  Mapping conn_;
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

}  // namespace
}  // namespace ibmw::selfdev::shm
