// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the findings of the first independent code review
// (RG-xx, numbered after the finding). Hostile-publisher cases use a raw
// protocol publisher, the counterpart of RawSubscriber in robustness_test.

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
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

constexpr uint32_t kRawStride = kChunkHeaderSize + 128;
constexpr uint32_t kRawChunks = 4;
constexpr uint32_t kRawPayload = 64;

std::string HashPrefix(const std::string& url) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016" PRIx64, Hash64(url));
  return buf;
}

void PublishSeq(Publisher& pub, uint64_t seq, uint32_t size = 64) {
  auto l = pub.Loan(size);
  ASSERT_TRUE(l.ok()) << ErrorName(l.error());
  FillPattern(*l, size, seq);
  pub.Publish(*l);
}

int CountOpenFds() {
  int n = 0;
  if (DIR* d = ::opendir("/proc/self/fd")) {
    while (auto* de = ::readdir(d)) n += de->d_name[0] != '.' ? 1 : 0;
    ::closedir(d);
  }
  return n;
}

// Unsealed memfd: what a hostile peer would pass to shrink it later.
UniqueFd UnsealedMemfd(size_t size) {
  UniqueFd fd(static_cast<int>(::syscall(SYS_memfd_create, "unsealed", MFD_CLOEXEC)));
  if (fd.Valid() && ::ftruncate(fd.Get(), static_cast<off_t>(size)) != 0) fd.Reset();
  return fd;
}

// A publisher implemented directly on the wire protocol: one data segment
// (id 0) of kRawChunks chunks, handles written straight into the ring.
class RawPublisher {
 public:
  ~RawPublisher() {
    if (!path_.empty()) ::unlink(path_.c_str());
  }

  bool Listen(const std::string& url) {
    std::string dir = Runtime::ServiceDirectory();
    char name[96];
    std::snprintf(name, sizeof(name), "%s.%d.%08x.sock", HashPrefix(url).c_str(), static_cast<int>(::getpid()),
                  static_cast<uint32_t>(RandomU64()));
    path_ = dir + "/" + name;
    std::string tmp = dir + "/." + name + ".tmp";
    listen_ = UniqueFd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, tmp.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    if (::listen(listen_.Get(), 4) != 0) return false;
    return ::rename(tmp.c_str(), path_.c_str()) == 0;  // IN_MOVED_TO: subscribers connect
  }

  // Accepts one subscriber, answers its Hello and announces segment 0.
  bool Handshake(bool seal_conn = true) {
    struct pollfd pfd {listen_.Get(), POLLIN, 0};
    if (::poll(&pfd, 1, 3000) <= 0) return false;
    sock_ = UniqueFd(::accept4(listen_.Get(), nullptr, nullptr, SOCK_CLOEXEC));
    std::vector<uint8_t> buf;
    std::vector<UniqueFd> fds;
    if (Receive(&buf, &fds) <= 0 || fds.size() != 1) return false;
    HelloMsg hello{};
    std::memcpy(&hello, buf.data(), std::min(buf.size(), sizeof(hello)));
    if (hello.h.type != MsgType::kHello) return false;
    cap = hello.queue_capacity;
    dslots = cap + 1;
    rslots = cap + hello.max_held + 1;
    size_t size = ConnSegmentSize(dslots, rslots);
    UniqueFd conn_fd;
    if (seal_conn) {
      auto seg = CreateSegment("raw_conn", size, false, true);
      if (!seg) return false;
      conn_ = std::move(seg.value().map);
      conn_fd = std::move(seg.value().fd);
    } else {
      conn_fd = UnsealedMemfd(size);
      void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, conn_fd.Get(), 0);
      if (p == MAP_FAILED) return false;
      conn_ = Mapping(p, size);
    }
    hdr = new (conn_.Data()) ConnHeader{};
    hdr->magic = kConnMagic;
    hdr->abi = kAbiVersion;
    hdr->queue_capacity = cap;
    hdr->delivery_slots = dslots;
    hdr->return_slots = rslots;
    hdr->notify_slot.store(kNoListener);
    WelcomeMsg wm{};
    wm.h = {kMsgMagic, MsgType::kWelcome};
    wm.abi = kAbiVersion;
    wm.queue_capacity = cap;
    wm.delivery_slots = dslots;
    wm.return_slots = rslots;
    wm.segment_size = size;
    int fd = conn_fd.Get();
    if (!SendMessage(sock_.Get(), &wm, sizeof(wm), &fd, 1)) return false;
    if (!seal_conn) return true;
    if (Receive(&buf, &fds) <= 0) return false;  // ConnAck
    auto data = CreateSegment("raw_data", kRawStride * kRawChunks, false, true);
    if (!data) return false;
    data_ = std::move(data.value());
    return SendSegmentAdd(0) && Receive(&buf, &fds) > 0;  // SegmentAck
  }

  bool SendSegmentAdd(uint32_t id) {
    SegmentAddMsg sm{};
    sm.h = {kMsgMagic, MsgType::kSegmentAdd};
    sm.segment_id = id;
    sm.chunk_count = kRawChunks;
    sm.chunk_stride = kRawStride;
    sm.segment_size = kRawStride * kRawChunks;
    int fd = data_.fd.Get();
    return SendMessage(sock_.Get(), &sm, sizeof(sm), &fd, 1);
  }

  // Writes chunk `idx` with the test pattern for `seq` and enqueues it.
  uint64_t Deliver(uint32_t idx, uint64_t seq) {
    uint8_t* base = data_.map.Data() + idx * kRawStride;
    auto* ch = reinterpret_cast<ChunkHeader*>(base);
    std::memset(ch, 0, sizeof(*ch));
    ch->magic = kChunkMagic;
    ch->abi = kAbiVersion;
    ch->chunk_index = idx;
    ch->generation = 1;
    ch->payload_size = kRawPayload;
    ch->payload_offset = kChunkHeaderSize;
    ch->payload_alignment = 8;
    ch->back_offset = kChunkHeaderSize;
    FillPattern(base + kChunkHeaderSize, kRawPayload, seq);
    uint64_t handle = MakeHandle(0, idx, 1);
    uint64_t w = hdr->delivery_write.load();
    DeliverySlots(hdr)[w % dslots].store(handle);
    hdr->delivery_write.store(w + 1);
    return handle;
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
  uint32_t cap = 0, dslots = 0, rslots = 0;

 private:
  ssize_t Receive(std::vector<uint8_t>* buf, std::vector<UniqueFd>* fds) {
    struct pollfd pfd {sock_.Get(), POLLIN, 0};
    if (::poll(&pfd, 1, 3000) <= 0) return -1;
    buf->assign(kMaxMsgSize, 0);
    fds->clear();
    ssize_t n = ReceiveMessage(sock_.Get(), buf->data(), buf->size(), fds);
    return n < 0 ? 0 : n;
  }

  std::string path_;
  UniqueFd listen_;
  UniqueFd sock_;
  Mapping conn_;
  Segment data_;
};

// RG-01 (#1) Destroying a Listener from its own callback is safe: the loop
// unwinds on a still-alive object and the slot is freed afterwards.
TEST(ShmRegressionTest, ListenerDestroyedFromOwnCallback) {
  auto url = UniqueUrl("rg01");
  PublisherOptions po;
  po.pools = {{64, 8}};
  Publisher pub(url, po);
  Subscriber sub(url);
  struct Ctx {
    Listener* listener;
    std::atomic<bool> done{false};
  } ctx{new Listener()};
  ASSERT_TRUE(ctx.listener->Attach(
      sub,
      [](Subscriber* s, void* c) {
        auto* x = static_cast<Ctx*>(c);
        while (auto t = s->Take()) s->Release(*t);
        delete x->listener;
        x->listener = nullptr;
        x->done = true;
      },
      &ctx));
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return ctx.done.load(); }));
  // The slot comes back once the detached thread has exited, and the
  // subscriber can be attached to a new listener.
  Listener again;
  ASSERT_TRUE(WaitFor([&] { return again.Attach(sub, [](Subscriber*, void*) {}, nullptr).ok(); }));
}

// RG-02 (#2) Destroying a Publisher while another thread is blocked in a
// lossless Publish() waits for that call to leave before freeing anything.
TEST(ShmRegressionTest, PublisherDestroyedWhileBlockedInPublish) {
  auto url = UniqueUrl("rg02");
  PublisherOptions po;
  po.too_slow = TooSlowPolicy::kWaitForConsumer;
  po.pools = {{64, 8}};
  auto pub = std::make_unique<Publisher>(url, po);
  Subscriber sub(url, SubscriberOptions{1, QueueFullPolicy::kBlockProducer, 4});
  ASSERT_TRUE(WaitFor([&] { return pub->HasSubscribers(); }));
  PublishSeq(*pub, 1);  // fills the queue
  std::atomic<int> state{0};
  std::thread t([&] {
    auto l = pub->Loan(64);
    state = 1;
    pub->Publish(*l);  // blocks: queue full, nobody takes
    state = 2;
  });
  ASSERT_TRUE(WaitFor([&] { return state.load() == 1 && pub->Stats().blocked == 1; }));
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(state.load(), 1);
  pub.reset();  // unblocks the publishing thread and waits for it (ASan: no UAF)
  t.join();
  EXPECT_EQ(state.load(), 2);
}

// RG-03a (#3) MapSegment only accepts regular files sealed against shrinking.
TEST(ShmRegressionTest, MapSegmentRequiresShrinkSeal) {
  auto unsealed = UnsealedMemfd(4096);
  ASSERT_TRUE(unsealed.Valid());
  auto m = MapSegment(unsealed.Get(), 4096, true);
  ASSERT_FALSE(m.ok());
  EXPECT_EQ(m.error(), Error::kCorrupted);

  int p[2];
  ASSERT_EQ(::pipe2(p, O_CLOEXEC), 0);
  UniqueFd r(p[0]), w(p[1]);
  EXPECT_FALSE(MapSegment(r.Get(), 4096, false).ok());

  auto sealed = CreateSegment("sealed", 4096, false, false);
  ASSERT_TRUE(sealed.ok());
  EXPECT_TRUE(MapSegment(sealed->fd.Get(), 4096, true).ok());
}

// RG-03b (#3) A subscriber passing an unsealed notify segment is rejected,
// so it can no longer SIGBUS the publisher by truncating it.
TEST(ShmRegressionTest, PublisherRejectsUnsealedNotifySegment) {
  auto url = UniqueUrl("rg03b");
  PublisherOptions po;
  po.pools = {{64, 8}};
  Publisher pub(url, po);
  std::string path;
  {
    std::string prefix = HashPrefix(url) + ".";
    DIR* d = ::opendir(Runtime::ServiceDirectory().c_str());
    while (auto* de = ::readdir(d)) {
      std::string n = de->d_name;
      if (n.rfind(prefix, 0) == 0) path = Runtime::ServiceDirectory() + "/" + n;
    }
    ::closedir(d);
  }
  ASSERT_FALSE(path.empty());
  UniqueFd s(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  ASSERT_EQ(::connect(s.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  auto notify = UnsealedMemfd(kNotifySegmentSize);
  auto* ns = static_cast<NotifySegment*>(
      ::mmap(nullptr, kNotifySegmentSize, PROT_READ | PROT_WRITE, MAP_SHARED, notify.Get(), 0));
  ASSERT_NE(ns, MAP_FAILED);
  ns->magic = kNotifyMagic;
  HelloMsg h{};
  h.h = {kMsgMagic, MsgType::kHello};
  h.abi = kAbiVersion;
  h.queue_capacity = 4;
  h.max_held = 4;
  h.url_length = static_cast<uint32_t>(url.size());
  std::memcpy(h.url, url.data(), url.size());
  int fd = notify.Get();
  ASSERT_TRUE(SendMessage(s.Get(), &h, offsetof(HelloMsg, url) + url.size(), &fd, 1));
  struct pollfd pfd {s.Get(), POLLIN, 0};
  ASSERT_EQ(::poll(&pfd, 1, 3000), 1);
  std::vector<uint8_t> buf(kMaxMsgSize);
  std::vector<UniqueFd> fds;
  ASSERT_GT(ReceiveMessage(s.Get(), buf.data(), buf.size(), &fds), 0);
  RejectMsg rm{};
  std::memcpy(&rm, buf.data(), sizeof(rm));
  EXPECT_EQ(rm.h.type, MsgType::kReject);
  EXPECT_EQ(rm.reason, static_cast<uint32_t>(Error::kCorrupted));
  ::munmap(ns, kNotifySegmentSize);
  ASSERT_EQ(::ftruncate(notify.Get(), 0), 0);  // the attack: must not hurt
  PublishSeq(pub, 1);
  EXPECT_EQ(pub.Stats().connections, 0u);
}

// RG-03c (#3) The reverse direction: an unsealed connection segment from a
// publisher is refused by the subscriber.
TEST(ShmRegressionTest, SubscriberRejectsUnsealedConnSegment) {
  auto url = UniqueUrl("rg03c");
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url));
  Subscriber sub(url, SubscriberOptions{4, QueueFullPolicy::kDiscardOldest, 4});
  ASSERT_TRUE(raw.Handshake(/*seal_conn=*/false));
  EXPECT_TRUE(raw.Closed());
  EXPECT_EQ(sub.Stats().connections, 0u);
}

// RG-04a (#4) Lossless publishing from many threads keeps every return ring
// within bounds: no parked returns, no pool growth from leaked chunks.
TEST(ShmRegressionTest, LosslessReturnRingNeverOverflows) {
  auto url = UniqueUrl("rg04a");
  PublisherOptions po;
  po.too_slow = TooSlowPolicy::kWaitForConsumer;
  Publisher pub(url, po);
  Subscriber sub(url, SubscriberOptions{1, QueueFullPolicy::kBlockProducer, 1});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> published{1};
  std::vector<std::thread> producers;
  for (int i = 0; i < 8; ++i) {
    producers.emplace_back([&] {
      while (!stop) {
        auto l = pub.Loan(8);
        if (!l) continue;  // loan limit shared by the 8 threads
        pub.Publish(*l);
        published.fetch_add(1);
      }
    });
  }
  uint64_t taken = 0;
  auto take_for = [&](std::chrono::milliseconds d) {
    auto end = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < end) {
      if (auto t = sub.Take()) {
        sub.Release(*t);
        ++taken;
      }
    }
  };
  take_for(1500ms);
  stop = true;
  take_for(200ms);  // lets blocked producers finish
  for (auto& t : producers) t.join();
  take_for(50ms);
  EXPECT_EQ(taken, published.load());
  EXPECT_EQ(sub.Stats().return_ring_full, 0u);
  EXPECT_EQ(pub.Stats().bad_returns, 0u);
  // 8 loans + 1 queued + 1 held + the one being published fit in 16 chunks.
  EXPECT_LE(pub.Stats().segments, 3u);
}

// RG-04b (#4) If a publisher does not drain its return ring the subscriber
// parks returns instead of dropping them, and flushes them in order later.
TEST(ShmRegressionTest, SubscriberParksReturnsWhenRingFull) {
  auto url = UniqueUrl("rg04b");
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url));
  Subscriber sub(url, SubscriberOptions{4, QueueFullPolicy::kDiscardOldest, 4});
  ASSERT_TRUE(raw.Handshake());
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 1; }));
  std::vector<uint64_t> delivered;
  auto round = [&](uint64_t seq) {
    delivered.push_back(raw.Deliver(static_cast<uint32_t>(seq % kRawChunks), seq));
    auto t = sub.Take();
    ASSERT_TRUE(t.ok());
    EXPECT_EQ(CheckPattern(*t, kRawPayload), seq);
    sub.Release(*t);
  };
  const uint32_t extra = 3;
  for (uint64_t s = 0; s < raw.rslots + extra; ++s) round(s);
  EXPECT_EQ(raw.hdr->return_write.load(), raw.rslots);
  EXPECT_EQ(sub.Stats().return_ring_full, extra);
  // The publisher finally drains; the next release flushes the backlog.
  auto* slots = ReturnSlots(raw.hdr, raw.dslots);
  std::vector<uint64_t> returned;
  for (uint64_t i = 0; i < raw.rslots; ++i) returned.push_back(slots[i % raw.rslots].load());
  raw.hdr->return_read.store(raw.rslots);
  round(raw.rslots + extra);
  uint64_t w = raw.hdr->return_write.load();
  EXPECT_EQ(w, raw.rslots + extra + 1);
  for (uint64_t i = raw.rslots; i < w; ++i) returned.push_back(slots[i % raw.rslots].load());
  EXPECT_EQ(returned, delivered);
}

// RG-05 (#5) Samples queued when a publisher shuts down cleanly can still be
// taken, in order.
TEST(ShmRegressionTest, QueuedSamplesSurvivePublisherShutdown) {
  auto url = UniqueUrl("rg05");
  Subscriber sub(url, SubscriberOptions{8, QueueFullPolicy::kDiscardOldest, 8});
  auto pub = std::make_unique<Publisher>(url);
  ASSERT_TRUE(WaitFor([&] { return pub->HasSubscribers(); }));
  PublishSeq(*pub, 1);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  for (uint64_t s = 2; s <= 4; ++s) PublishSeq(*pub, s);
  pub.reset();
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 0; }));
  EXPECT_TRUE(sub.HasData());
  std::vector<uint64_t> got;
  std::vector<const void*> held;
  while (auto t = sub.Take()) {
    got.push_back(CheckPattern(*t, 64));
    held.push_back(*t);
  }
  EXPECT_EQ(got, (std::vector<uint64_t>{1, 2, 3, 4}));
  EXPECT_FALSE(sub.HasData());
  for (auto* p : held) sub.Release(p);
  EXPECT_EQ(sub.Stats().held, 0u);
}

// RG-07 (#7) A delivery write index far ahead of the ring is corruption: the
// subscriber drops the publisher instead of looping under its lock.
TEST(ShmRegressionTest, HostileDeliveryIndexDropsPublisher) {
  auto url = UniqueUrl("rg07");
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url));
  Subscriber sub(url, SubscriberOptions{4, QueueFullPolicy::kDiscardOldest, 4});
  ASSERT_TRUE(raw.Handshake());
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 1; }));
  raw.hdr->delivery_write.store(1ull << 40);
  auto t0 = std::chrono::steady_clock::now();
  auto r = sub.Take();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 100ms);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(sub.Stats().corrupted_connections, 1u);
  EXPECT_TRUE(raw.Closed());
  EXPECT_FALSE(sub.HasData());
}

// RG-08 (#8) Re-announcing a segment id drops the publisher; the mapping
// behind an already taken sample stays valid until it is released.
TEST(ShmRegressionTest, DuplicateSegmentAddDropsPublisher) {
  auto url = UniqueUrl("rg08");
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url));
  Subscriber sub(url, SubscriberOptions{4, QueueFullPolicy::kDiscardOldest, 4});
  ASSERT_TRUE(raw.Handshake());
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 1; }));
  raw.Deliver(0, 42);
  auto t = sub.Take();
  ASSERT_TRUE(t.ok());
  ASSERT_TRUE(raw.SendSegmentAdd(0));
  EXPECT_TRUE(raw.Closed());
  EXPECT_EQ(sub.Stats().corrupted_connections, 1u);
  EXPECT_EQ(CheckPattern(*t, kRawPayload), 42u);
  sub.Release(*t);
  EXPECT_EQ(sub.Stats().held, 0u);
}

// RG-11 (#11) Tearing down a subscriber and its listener concurrently is
// race free (the subscriber/listener link is guarded; checked under TSan).
TEST(ShmRegressionTest, ConcurrentSubscriberAndListenerTeardown) {
  auto url = UniqueUrl("rg11");
  for (int i = 0; i < 50; ++i) {
    auto sub = std::make_unique<Subscriber>(url);
    auto listener = std::make_unique<Listener>();
    ASSERT_TRUE(listener->Attach(*sub, [](Subscriber*, void*) {}, nullptr));
    std::thread a([&] { sub.reset(); });
    std::thread b([&] { listener.reset(); });
    a.join();
    b.join();
  }
}

// RG-13 (#13) A connection costs one socket per side; the publisher does not
// keep the connection segment's descriptor.
TEST(ShmRegressionTest, ConnectionDoesNotRetainSegmentFd) {
  auto url = UniqueUrl("rg13");
  PublisherOptions po;
  po.pools = {{64, 8}};
  Publisher pub(url, po);
  Subscriber first(url);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 1; }));
  const int before = CountOpenFds();
  std::vector<std::unique_ptr<Subscriber>> subs;
  for (int i = 0; i < 10; ++i) subs.push_back(std::make_unique<Subscriber>(url));
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 11; }));
  EXPECT_EQ(CountOpenFds() - before, 2 * 10);
}

// RG-14 (found by the extension tests) A lossless subscriber's queue capacity
// covers samples parked for an unacknowledged new segment too: a publisher
// whose pool grows while the ring is full blocks instead of buffering more.
TEST(ShmRegressionTest, LosslessCapacityIncludesPendingSegments) {
  auto url = UniqueUrl("rg14");
  PublisherOptions po;
  po.too_slow = TooSlowPolicy::kWaitForConsumer;
  po.block_timeout = 50ms;
  po.pools = {{64, 16}};
  Publisher pub(url, po);
  Subscriber sub(url, SubscriberOptions{4, QueueFullPolicy::kBlockProducer, 4});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  auto publish = [&](uint32_t size, uint64_t seq) {
    auto l = pub.Loan(size);
    ASSERT_TRUE(l.ok()) << ErrorName(l.error());
    test::FillPattern(*l, size, seq);
    pub.Publish(*l);
  };
  for (uint64_t s = 1; s <= 4; ++s) publish(64, s);  // ring full
  // Each new size class creates a segment the subscriber has not mapped yet.
  for (uint64_t s = 5; s <= 8; ++s) publish(static_cast<uint32_t>(64u << (s - 4)), s);
  auto st = pub.Stats();
  EXPECT_EQ(st.deliveries, 4u);
  EXPECT_EQ(st.block_timeouts, 4u);
  for (uint64_t s = 1; s <= 4; ++s) {
    auto t = sub.Take();
    ASSERT_TRUE(t.ok());
    EXPECT_EQ(CheckPattern(*t, 64), s);
    sub.Release(*t);
  }
  EXPECT_FALSE(sub.Take().ok());
}

// RG-15 (found by PS-16 repeats) Take() loaded the read index before the write
// index; an evicting publisher can advance both in between, so a healthy ring
// looked more than capacity behind and the subscriber dropped it as corrupt.
TEST(ShmRegressionTest, EvictionRaceIsNotCorruption) {
  auto url = UniqueUrl("rg15");
  Publisher pub(url);
  Subscriber sub(url, SubscriberOptions{2, QueueFullPolicy::kDiscardOldest, 4});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  constexpr int kThreads = 2, kPerThread = 100000;
  std::atomic<bool> done{false};
  std::atomic<uint64_t> taken{0};
  std::thread consumer([&] {
    while (!done.load()) {
      auto t = sub.Take();
      if (!t) continue;
      sub.Release(*t);
      taken.fetch_add(1);
    }
  });
  std::vector<std::thread> producers;
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back([&] {
      for (int i = 0; i < kPerThread; ++i) {
        auto l = pub.Loan(64);
        if (!l) {
          --i;
          continue;
        }
        pub.Publish(*l);
      }
    });
  }
  for (auto& p : producers) p.join();
  EXPECT_TRUE(WaitFor([&] {
    auto st = pub.Stats();
    return taken.load() + st.evictions == st.deliveries;
  }));
  done = true;
  consumer.join();
  EXPECT_EQ(sub.Stats().corrupted_connections, 0u);
  EXPECT_EQ(pub.Stats().corrupted_connections, 0u);
  EXPECT_EQ(sub.Stats().connections, 1u);
}

// RG-16 (second review, H1) The subscriber reports the payload size it
// validated in Take(); rewriting the header afterwards (the publisher can,
// it is publisher-writable memory) does not change it.
TEST(ShmRegressionTest, PayloadSizeIsTheValidatedSize) {
  auto url = UniqueUrl("rg16");
  Publisher pub(url);
  Subscriber sub(url);
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  auto l = pub.Loan(100);
  ASSERT_TRUE(l.ok());
  void* chunk = *l;
  auto* h = const_cast<ChunkHeader*>(ChunkView::FromUserPayload(chunk).Header());
  pub.Publish(chunk);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto t = sub.Take();
  ASSERT_TRUE(t.ok());
  EXPECT_EQ(sub.PayloadSize(*t), 100u);
  const ChunkHeader saved = *h;
  h->payload_size = 1u << 30;
  h->back_offset = 0xdeadbeef;
  EXPECT_EQ(sub.PayloadSize(*t), 100u);
  *h = saved;
  sub.Release(*t);
  EXPECT_EQ(sub.PayloadSize(*t), 0u);
  int local = 0;
  EXPECT_EQ(sub.PayloadSize(&local), 0u);
  EXPECT_EQ(sub.PayloadSize(nullptr), 0u);
}

// RG-17 (second review, M1) Publish/Release report whether they ended a
// loan, so a caller counting its loans cannot be driven out of step.
TEST(ShmRegressionTest, PublishReleaseReportUnknownChunks) {
  Publisher pub(UniqueUrl("rg17"));
  auto a = pub.Loan(64);
  auto b = pub.Loan(64);
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_TRUE(pub.Release(*a));
  EXPECT_FALSE(pub.Release(*a));
  EXPECT_FALSE(pub.Publish(*a));
  EXPECT_TRUE(pub.Publish(*b));
  EXPECT_FALSE(pub.Publish(*b));
  EXPECT_FALSE(pub.Release(*b));
  int local = 0;
  EXPECT_FALSE(pub.Publish(&local));
  EXPECT_FALSE(pub.Release(&local));
  EXPECT_FALSE(pub.Publish(nullptr));
  EXPECT_EQ(pub.Stats().publishes, 1u);
}

// RG-18 (second review, M2) StopBlocking releases a Publish waiting without
// timeout on a lossless subscriber that never takes, and later publishes do
// not block either.
TEST(ShmRegressionTest, StopBlockingReleasesBlockedPublish) {
  auto url = UniqueUrl("rg18");
  PublisherOptions po;
  po.too_slow = TooSlowPolicy::kWaitForConsumer;
  Publisher pub(url, po);
  Subscriber sub(url, SubscriberOptions{1, QueueFullPolicy::kBlockProducer, 4});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  auto first = pub.Loan(64);
  ASSERT_TRUE(first.ok());
  pub.Publish(*first);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));  // queue (1) is full
  std::atomic<bool> returned{false};
  // Unblocks the publisher on any exit, so a failed ASSERT reports instead
  // of hanging in the join.
  struct Unblock {
    Publisher& pub;
    ~Unblock() { pub.StopBlocking(); }
  };
  std::jthread blocked([&] {
    auto l = pub.Loan(64);
    if (l) pub.Publish(*l);
    returned = true;
  });
  Unblock unblock{pub};
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().blocked >= 1; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(returned.load());
  pub.StopBlocking();
  blocked.join();
  EXPECT_GE(pub.Stats().block_timeouts, 1u);
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 10; ++i) {
    auto l = pub.Loan(64);
    ASSERT_TRUE(l.ok());
    pub.Publish(*l);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 5s);
  EXPECT_GE(pub.Stats().block_timeouts, 11u);
  // The subscriber still gets what was queued before.
  auto t = sub.Take();
  ASSERT_TRUE(t.ok());
  sub.Release(*t);
}

// RG-19 (second review, M5) Init() on a running runtime with a different
// domain fails instead of silently keeping the first one.
TEST(ShmRegressionTest, ConflictingInitIsRejected) {
  ASSERT_TRUE(Runtime::IsInitialized());
  const std::string dir = Runtime::ServiceDirectory();
  EXPECT_TRUE(Runtime::Init(RuntimeOptions{}).ok());
  RuntimeOptions other;
  other.domain = "rg19_other";
  auto r = Runtime::Init(other);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error(), Error::kAlreadyInitialized);
  RuntimeOptions other_root;
  other_root.root_dir = "/nonexistent_rg19";
  EXPECT_EQ(Runtime::Init(other_root).error(), Error::kAlreadyInitialized);
  RuntimeOptions invalid;
  invalid.domain = "a/b";
  EXPECT_EQ(Runtime::Init(invalid).error(), Error::kInvalidArg);
  EXPECT_EQ(Runtime::ServiceDirectory(), dir);
}

}  // namespace
}  // namespace ibmw::selfdev::shm
