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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ibmw_selfdev/shm/shm.h"
#include "platform.h"
#include "protocol.h"
#include "runtime_impl.h"
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
    if (!tmp_.empty()) ::unlink(tmp_.c_str());
  }

  // Creates the listening socket; with `announce` it is also renamed to its
  // public name, which is what subscribers discover.
  bool Listen(const std::string& url, bool announce = true) {
    std::string dir = Runtime::ServiceDirectory();
    char name[96];
    std::snprintf(name, sizeof(name), "%s.%d.%08x.sock", HashPrefix(url).c_str(), static_cast<int>(::getpid()),
                  static_cast<uint32_t>(RandomU64()));
    path_ = dir + "/" + name;
    tmp_ = dir + "/." + name + ".tmp";
    const std::string& tmp = tmp_;
    listen_ = UniqueFd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, tmp.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(listen_.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
    if (::listen(listen_.Get(), 4) != 0) return false;
    return !announce || Announce();
  }
  // Accepts and closes `n` queued connections (in arrival order).
  size_t AcceptAndDrop(size_t n) {
    size_t done = 0;
    for (; done < n; ++done) {
      struct pollfd pfd {listen_.Get(), POLLIN, 0};
      if (::poll(&pfd, 1, 3000) <= 0) break;
      UniqueFd c(::accept4(listen_.Get(), nullptr, nullptr, SOCK_CLOEXEC));
      if (!c.Valid()) break;
    }
    return done;
  }
  bool Announce() { return ::rename(tmp_.c_str(), path_.c_str()) == 0; }  // IN_MOVED_TO: subscribers connect
  const std::string& PublicPath() const { return path_; }
  const std::string& ListenPath() const { return tmp_; }

  // Accepts one subscriber and reads its Hello.
  bool AcceptHello() {
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
    return true;
  }

  // Maps a connection segment and sends it with the Welcome.
  bool SendWelcome(bool seal_conn = true) {
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
    return SendMessage(sock_.Get(), &wm, sizeof(wm), &fd, 1);
  }

  // A message from the subscriber is waiting.
  bool Readable() {
    struct pollfd pfd {sock_.Get(), POLLIN, 0};
    return ::poll(&pfd, 1, 0) > 0;
  }

  // Accepts one subscriber, answers its Hello and announces segment 0.
  bool Handshake(bool seal_conn = true) {
    if (!AcceptHello() || !SendWelcome(seal_conn)) return false;
    if (!seal_conn) return true;
    std::vector<uint8_t> buf;
    std::vector<UniqueFd> fds;
    if (Receive(&buf, &fds) <= 0) return false;  // ConnAck
    auto data = CreateSegment("raw_data", kRawStride * kRawChunks, false, true);
    if (!data) return false;
    data_ = std::move(data.value());
    return SendSegmentAdd(0) && Receive(&buf, &fds) > 0;  // SegmentAck
  }

  // Sends `msg`, waiting while the subscriber has not read earlier ones.
  bool SendBlocking(const void* msg, size_t len, int fd = -1) {
    for (;;) {
      if (SendMessage(sock_.Get(), msg, len, fd >= 0 ? &fd : nullptr, fd >= 0 ? 1 : 0)) return true;
      if (!SendWouldBlock(errno)) return false;
      struct pollfd pfd {sock_.Get(), POLLOUT, 0};
      if (::poll(&pfd, 1, 3000) <= 0) return false;
    }
  }

  bool SendSegmentAddBlocking(uint32_t id) {
    SegmentAddMsg sm{};
    sm.h = {kMsgMagic, MsgType::kSegmentAdd};
    sm.segment_id = id;
    sm.chunk_count = kRawChunks;
    sm.chunk_stride = kRawStride;
    sm.segment_size = kRawStride * kRawChunks;
    return SendBlocking(&sm, sizeof(sm), data_.fd.Get());
  }

  // Reads messages until `n` of `type` arrived; returns how many did.
  int Expect(MsgType type, int n) {
    std::vector<uint8_t> buf;
    std::vector<UniqueFd> fds;
    int got = 0;
    while (got < n) {
      ssize_t len = Receive(&buf, &fds);
      if (len < static_cast<ssize_t>(sizeof(MsgHeader))) break;
      MsgHeader mh{};
      std::memcpy(&mh, buf.data(), sizeof(mh));
      if (mh.type == type) ++got;
    }
    return got;
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
  std::string tmp_;
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
  test::JoiningThread blocked([&] {
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

// RG-20 (third review, H1/M6) A full control socket is backpressure, not a broken
// connection. The publisher's send of a new segment would block; it is
// queued and sent on EPOLLOUT, and a sample the subscriber holds survives.
TEST(ShmRegressionTest, PublisherQueuesControlMessagesOnBackpressure) {
  auto url = UniqueUrl("rg20");
  Publisher pub(url);
  Subscriber sub(url);
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto held = sub.Take();
  ASSERT_TRUE(held.ok());
  fault::send_would_block.store(3);
  auto big = pub.Loan(4096);  // new size class: a SegmentAdd that would block
  fault::send_would_block.store(0);
  ASSERT_TRUE(big.ok()) << ErrorName(big.error());
  FillPattern(*big, 4096, 2);
  ASSERT_TRUE(pub.Publish(*big));
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto second = sub.Take();
  ASSERT_TRUE(second.ok()) << ErrorName(second.error());
  EXPECT_EQ(CheckPattern(*second, 4096), 2u);
  EXPECT_EQ(CheckPattern(*held, 64), 1u);
  auto st = pub.Stats();
  EXPECT_GT(st.deferred_sends, 0u);
  EXPECT_EQ(st.connections, 1u);
  EXPECT_EQ(st.reclaimed, 0u);
  EXPECT_EQ(st.quarantined, 0u);
  EXPECT_EQ(sub.Stats().connections, 1u);
  sub.Release(*held);
  sub.Release(*second);
}

// RG-21 (third review, H1/M6), subscriber side: a publisher that announces many
// segments without reading the acks fills the subscriber's socket. The
// acks wait in the subscriber's queue; the connection stays and every ack
// arrives once the publisher reads.
TEST(ShmRegressionTest, SubscriberQueuesAcksOnBackpressure) {
  auto url = UniqueUrl("rg21");
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url));
  Subscriber sub(url);
  ASSERT_TRUE(raw.Handshake());
  constexpr uint32_t kSegments = 1500;  // far more acks than a socket buffer holds
  for (uint32_t id = 1; id <= kSegments; ++id) ASSERT_TRUE(raw.SendSegmentAddBlocking(id)) << id;
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().deferred_sends > 0; }));
  EXPECT_EQ(raw.Expect(MsgType::kSegmentAck, static_cast<int>(kSegments)), static_cast<int>(kSegments));
  auto st = sub.Stats();
  EXPECT_EQ(st.connections, 1u);
  EXPECT_EQ(st.send_queue_overflows, 0u);
  EXPECT_EQ(st.corrupted_connections, 0u);
  raw.Deliver(0, 7);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto t = sub.Take();
  ASSERT_TRUE(t.ok()) << ErrorName(t.error());
  EXPECT_EQ(CheckPattern(*t, kRawPayload), 7u);
  sub.Release(*t);
}

// RG-22 (third review, H1) Control messages of a type this version does not know
// come from a newer, compatible peer; they are counted and ignored.
TEST(ShmRegressionTest, UnknownPublisherMessageIsIgnored) {
  auto url = UniqueUrl("rg22");
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url));
  Subscriber sub(url);
  ASSERT_TRUE(raw.Handshake());
  struct {
    MsgHeader h;
    uint32_t payload;
  } future{{kMsgMagic, static_cast<MsgType>(200)}, 42};
  ASSERT_TRUE(raw.SendBlocking(&future, sizeof(future)));
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().unknown_messages == 1; }));
  EXPECT_EQ(sub.Stats().connections, 1u);
  raw.Deliver(1, 3);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto t = sub.Take();
  ASSERT_TRUE(t.ok()) << ErrorName(t.error());
  EXPECT_EQ(CheckPattern(*t, kRawPayload), 3u);
  sub.Release(*t);
}

// RG-23 (third review, M1) connect() fails with EAGAIN while the publisher's listen
// backlog is full. The subscriber retries with backoff instead of giving up.
TEST(ShmRegressionTest, ConnectRetriesWhileBacklogIsFull) {
  auto url = UniqueUrl("rg23");
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url, false));
  std::vector<UniqueFd> fillers;
  for (int i = 0; i < 64; ++i) {
    UniqueFd fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, raw.ListenPath().c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK) << std::strerror(errno);
      break;
    }
    fillers.push_back(std::move(fd));
  }
  ASSERT_FALSE(fillers.empty());
  ASSERT_LT(fillers.size(), 64u);  // the backlog did fill up
  Subscriber sub(url);
  ASSERT_TRUE(raw.Announce());
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connect_retries > 0; }));
  // Accept (and drop) the fillers; the subscriber's retry gets through.
  ASSERT_EQ(raw.AcceptAndDrop(fillers.size()), fillers.size());
  fillers.clear();
  ASSERT_TRUE(raw.Handshake());
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 1; }));
  raw.Deliver(0, 5);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto t = sub.Take();
  ASSERT_TRUE(t.ok()) << ErrorName(t.error());
  EXPECT_EQ(CheckPattern(*t, kRawPayload), 5u);
  sub.Release(*t);
}

// Holds the control thread until destroyed, so tests can let inotify events
// pile up the way they do on a busy system.
class ControlThreadStall {
 public:
  ControlThreadStall() {
    auto f = release_.get_future().share();
    detail::RuntimeImpl::Instance()->Post([this, f] {
      stalled_.store(true);
      f.wait();
    });
    while (!stalled_.load()) std::this_thread::sleep_for(1ms);
  }
  ~ControlThreadStall() { release_.set_value(); }

 private:
  std::promise<void> release_;
  std::atomic<bool> stalled_{false};
};

size_t MaxQueuedInotifyEvents() {
  size_t n = 16384;
  if (FILE* f = std::fopen("/proc/sys/fs/inotify/max_queued_events", "r")) {
    unsigned long v = 0;
    if (std::fscanf(f, "%lu", &v) == 1 && v > 0) n = v;
    std::fclose(f);
  }
  return n;
}

// RG-24 (third review, M2) When the inotify queue overflows (IN_Q_OVERFLOW) the
// events it dropped may include a new publisher; the runtime rescans the
// whole directory instead of missing it.
TEST(ShmRegressionTest, InotifyOverflowTriggersRescan) {
  auto url = UniqueUrl("rg24");
  Subscriber sub(url);
  RawPublisher raw;
  {
    ControlThreadStall stall;
    const std::string dir = Runtime::ServiceDirectory();
    const std::string a = dir + "/rg24_a", b = dir + "/rg24_b";
    int fd = ::open(a.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    ASSERT_GE(fd, 0);
    ::close(fd);
    const size_t renames = MaxQueuedInotifyEvents() + 100;  // IN_MOVED_TO each
    for (size_t i = 0; i < renames; ++i) {
      ASSERT_EQ(::rename(i % 2 == 0 ? a.c_str() : b.c_str(), i % 2 == 0 ? b.c_str() : a.c_str()), 0);
    }
    ::unlink(a.c_str());
    ::unlink(b.c_str());
    ASSERT_TRUE(raw.Listen(url));  // its IN_MOVED_TO is lost in the overflow
  }
  ASSERT_TRUE(raw.Handshake());
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 1; }));
}

// Empties and removes the service directory (only stale entries of earlier
// tests are in it).
bool RemoveServiceDirectory() {
  const std::string dir = Runtime::ServiceDirectory();
  if (DIR* d = ::opendir(dir.c_str())) {
    while (auto* de = ::readdir(d)) {
      if (de->d_name[0] != '.' || (de->d_name[1] != '\0' && std::strcmp(de->d_name, "..") != 0)) {
        ::unlink((dir + "/" + de->d_name).c_str());
      }
    }
    ::closedir(d);
  }
  return ::rmdir(dir.c_str()) == 0;
}

// RG-25 (third review, M2) The watched service directory is removed (tmp cleaner,
// administrator). The runtime re-creates it, watches it again and rescans.
TEST(ShmRegressionTest, RemovedServiceDirectoryIsRecreated) {
  auto url = UniqueUrl("rg25");
  Subscriber sub(url);
  const std::string dir = Runtime::ServiceDirectory();
  ASSERT_TRUE(RemoveServiceDirectory()) << std::strerror(errno);
  struct stat st {};
  ASSERT_TRUE(WaitFor([&] { return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }));
  EXPECT_EQ(st.st_mode & 0777, 0700u);
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url));
  ASSERT_TRUE(raw.Handshake());
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 1; }));
}

// Runs "ibmw_selfdev_shm_peer probe" and returns its four answers.
struct ProbeResult {
  std::string init, dir, pub, sub, sock;
};
// `as`: the "as <uid> <gid> <groups>" prefix of a peer running as another user.
ProbeResult Probe(const std::vector<std::string>& args, const std::vector<std::string>& env,
                  const std::vector<std::string>& as = {}) {
  std::vector<std::string> full = as;
  full.push_back("probe");
  full.insert(full.end(), args.begin(), args.end());
  auto peer = test::SpawnPeer(full, env);
  ProbeResult r;
  auto value = [&](const char* key) {
    std::string line = test::ReadLine(peer);
    std::string k = std::string(key) + " ";
    return line.compare(0, k.size(), k) == 0 ? line.substr(k.size()) : "<" + line + ">";
  };
  r.init = value("init");
  r.dir = value("dir");
  r.pub = value("pub");
  r.sub = value("sub");
  r.sock = value("sock");
  test::WaitPeer(peer);
  return r;
}

std::string Euid() { return std::to_string(::geteuid()); }

// RG-26 (third review, M3/O6) Without root_dir and $IBMW_SHM_ROOT the root is a
// fixed path: /dev/shm when usable (tmpfs, not aged by tmpfiles), else
// /tmp. On OHOS it must be given explicitly.
TEST(ShmRegressionTest, DefaultRootIsFixed) {
  const std::string domain = "rg26_" + std::to_string(::getpid());
  auto r = Probe({UniqueUrl("rg26")}, {"IBMW_SHM_ROOT=", "IBMW_SHM_DOMAIN=" + domain, "XDG_RUNTIME_DIR=/nonexistent",
                                       "TMPDIR=/nonexistent"});
  EXPECT_EQ(r.init, "-");
#if defined(__OHOS__)
  EXPECT_EQ(r.pub, "invalid_arg");
  EXPECT_EQ(r.sub, "invalid_arg");
#else
  const std::string root = ::access("/dev/shm", W_OK | X_OK) == 0 ? "/dev/shm" : "/tmp";
  const std::string dir = root + "/ibmw_shm." + Euid() + "." + domain;
  EXPECT_EQ(r.dir, dir);
  EXPECT_EQ(r.pub, "ok");
  EXPECT_EQ(r.sub, "ok");
  struct stat st {};
  ASSERT_EQ(::stat(dir.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0700u);
  EXPECT_EQ(st.st_uid, ::geteuid());
  EXPECT_EQ(::rmdir(dir.c_str()), 0) << std::strerror(errno);
#endif
}

// RG-27 (third review, O7) Socket paths must fit sun_path, so the service directory
// is limited to 63 bytes. Beyond it Runtime, Publisher and Subscriber fail
// with kInvalidArg (and a message naming the limit) instead of every
// connection failing silently.
TEST(ShmRegressionTest, ServiceDirectoryLengthIsChecked) {
  const size_t fixed = std::string("/ibmw_shm.").size() + Euid().size() + 2;  // + ".d"
  ASSERT_LT(test::TestRoot().size() + fixed + 2, 63u);
  auto root_for = [&](size_t dir_len) {
    std::string root = test::TestRoot() + "/";
    root.append(dir_len - fixed - root.size(), 'r');
    ::mkdir(root.c_str(), 0700);
    return root;
  };
  const std::string ok_root = root_for(63), long_root = root_for(64);
  auto r = Probe({UniqueUrl("rg27")}, {"IBMW_SHM_ROOT=" + ok_root, "IBMW_SHM_DOMAIN=d"});
  EXPECT_EQ(r.dir.size(), 63u);
  EXPECT_EQ(r.pub, "ok");
  EXPECT_EQ(r.sub, "ok");
  r = Probe({UniqueUrl("rg27")}, {"IBMW_SHM_ROOT=" + long_root, "IBMW_SHM_DOMAIN=d"});
  EXPECT_EQ(r.pub, "invalid_arg");
  EXPECT_EQ(r.sub, "invalid_arg");
  r = Probe({UniqueUrl("rg27"), "d", long_root}, {});
  EXPECT_EQ(r.init, "invalid_arg");
  // A publisher with an unusable runtime must not exist half-way.
  struct stat st {};
  EXPECT_NE(::stat((long_root + "/ibmw_shm." + Euid() + ".d").c_str(), &st), 0);
}

// RG-28 (third review, M14) Root, domain and group follow one rule: explicit option,
// else environment variable, else default.
TEST(ShmRegressionTest, OptionThenEnvironmentThenDefault) {
  const std::string root = test::TestRoot();
  const std::string opt_root = root + "/rg28";
  ::mkdir(opt_root.c_str(), 0700);
  auto url = UniqueUrl("rg28");
  auto r = Probe({url, "optdom", opt_root}, {"IBMW_SHM_DOMAIN=envdom"});
  EXPECT_EQ(r.init, "ok");
  EXPECT_EQ(r.dir, opt_root + "/ibmw_shm." + Euid() + ".optdom");
  r = Probe({url, "-", opt_root}, {"IBMW_SHM_DOMAIN=envdom"});
  EXPECT_EQ(r.dir, opt_root + "/ibmw_shm." + Euid() + ".envdom");
  r = Probe({url}, {"IBMW_SHM_DOMAIN=envdom"});
  EXPECT_EQ(r.dir, root + "/ibmw_shm." + Euid() + ".envdom");
  r = Probe({url}, {"IBMW_SHM_DOMAIN="});
  EXPECT_EQ(r.dir, root + "/ibmw_shm." + Euid() + ".default");
  // Only plain digits are a gid: strtoul() would accept the signed and blank forms.
  for (const char* g : {"staff", "+5", " 5", "-1", "5 "}) {
    r = Probe({url}, {std::string("IBMW_SHM_GROUP=") + g});
    EXPECT_EQ(r.pub, "invalid_arg") << "'" << g << "'";
    EXPECT_EQ(r.sub, "invalid_arg") << "'" << g << "'";
  }
}

// RG-29 (third review, M15) Users only meet through a shared group. The group
// directory is g<gid>, mode 0770 and owned by that group; directories of
// the same domain under other owners are detected (and reported on
// stderr, since those processes can never meet this one).
TEST(ShmRegressionTest, GroupDirectoryAndForeignOwners) {
  const std::string root = test::TestRoot() + "/rg29";
  ::mkdir(root.c_str(), 0700);
  const gid_t gid = ::getegid();
  auto r = Probe({UniqueUrl("rg29")}, {"IBMW_SHM_ROOT=" + root, "IBMW_SHM_DOMAIN=dom",
                                       "IBMW_SHM_GROUP=" + std::to_string(gid)});
  const std::string gdir = root + "/ibmw_shm.g" + std::to_string(gid) + ".dom";
  EXPECT_EQ(r.dir, gdir);
  EXPECT_EQ(r.pub, "ok");
  EXPECT_EQ(r.sub, "ok");
  struct stat st {};
  ASSERT_EQ(::stat(gdir.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0770u);
  EXPECT_EQ(st.st_gid, gid);
  for (const char* n : {"ibmw_shm.4242.dom", "ibmw_shm.4242.other", "ibmw_shm.1.x.dom", "unrelated.dom"}) {
    ::mkdir((root + "/" + n).c_str(), 0700);
  }
  const std::string mine = "ibmw_shm." + Euid() + ".dom";
  ::mkdir((root + "/" + mine).c_str(), 0700);
  auto foreign = detail::ForeignServiceDirs(root, Euid(), "dom");
  std::sort(foreign.begin(), foreign.end());
  std::vector<std::string> expected{root + "/ibmw_shm.4242.dom", gdir};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(foreign, expected);
}

// RG-30 (third review, M4) Without F_SEAL_FUTURE_WRITE subscribers could map data
// segments writable. That is counted and visible; require_read_only turns
// it into a hard failure instead.
TEST(ShmRegressionTest, MissingWriteSealIsVisibleOrFatal) {
  struct Restore {
    ~Restore() { fault::no_future_write_seal.store(false); }
  } restore;
  fault::no_future_write_seal.store(true);
  {
    Publisher pub(UniqueUrl("rg30"));
    auto l = pub.Loan(64);
    ASSERT_TRUE(l.ok()) << ErrorName(l.error());
    pub.Release(*l);
    auto st = pub.Stats();
    EXPECT_FALSE(st.read_only_enforced);
    EXPECT_GE(st.unsealed_segments, 1u);
  }
  PublisherOptions strict;
  strict.require_read_only = true;
  Publisher pub(UniqueUrl("rg30"), strict);
  auto l = pub.Loan(64);
  ASSERT_FALSE(l.ok());
  EXPECT_EQ(l.error(), Error::kSystem);
  EXPECT_EQ(pub.Stats().segments, 0u);
  fault::no_future_write_seal.store(false);
  auto ok = pub.Loan(64);
  ASSERT_TRUE(ok.ok()) << ErrorName(ok.error());
  EXPECT_TRUE(pub.Stats().read_only_enforced);
  pub.Release(*ok);
}

// RG-31 (fourth review, H-1) Running out of memory while growing the pool is
// the publisher's problem, not a protocol violation: the subscriber loses
// the connection, but what it holds stays quarantined and is not handed to
// a later Loan().
TEST(ShmRegressionTest, GrowOutOfMemoryKeepsHeldChunks) {
  struct Restore {
    ~Restore() { fault::grow_bad_alloc.store(0); }
  } restore;
  auto url = UniqueUrl("rg31");
  Publisher pub(url);
  Subscriber sub(url);
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto held = sub.Take();
  ASSERT_TRUE(held.ok()) << ErrorName(held.error());

  fault::grow_bad_alloc.store(1);
  auto big = pub.Loan(4096);  // new size class: tracking this subscriber fails
  ASSERT_TRUE(big.ok()) << ErrorName(big.error());
  EXPECT_EQ(fault::grow_bad_alloc.load(), 0u);
  pub.Release(*big);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 0 && sub.Stats().connections == 0; }));
  auto st = pub.Stats();
  EXPECT_EQ(st.quarantined_chunks, 1u);
  EXPECT_EQ(st.reclaimed, 0u);

  // However often the pool is used up, the held chunk is never among it.
  for (int round = 0; round < 32; ++round) {
    std::vector<void*> loans;
    for (uint64_t i = 0; i < kMaxChunksAllocatedPerPublisher; ++i) {
      auto l = pub.Loan(64);
      ASSERT_TRUE(l.ok()) << ErrorName(l.error());
      FillPattern(*l, 64, 1000 + i);
      loans.push_back(*l);
    }
    for (auto* l : loans) {
      EXPECT_NE(l, *held);
      pub.Release(l);
    }
  }
  EXPECT_EQ(CheckPattern(*held, 64), 1u);

  // The last release is a late orderly close.
  sub.Release(*held);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().quarantined_chunks == 0; }));
  EXPECT_EQ(pub.Stats().reclaimed, 1u);
}

// RG-32 (fourth review, M-a) An exception in a control-thread command or timer
// stays there: Call() hands it to its caller instead of leaving it waiting
// for ever, Post() and RunAfter() log it, and the thread keeps running.
TEST(ShmRegressionTest, ControlThreadSurvivesExceptions) {
  auto* rt = detail::RuntimeImpl::Instance();
  ASSERT_NE(rt, nullptr);
  EXPECT_THROW(rt->Call([] { throw std::runtime_error("from Call"); }), std::runtime_error);
  // On the control thread Call() runs inline; the command around it catches.
  rt->Post([rt] { rt->Call([] { throw std::bad_alloc(); }); });
  rt->Post([] { throw 42; });
  std::atomic<bool> fired{false};
  rt->Call([&] {
    rt->RunAfter(1, [] { throw std::runtime_error("from a timer"); });
    rt->RunAfter(2, [&fired] { fired.store(true); });
  });
  ASSERT_TRUE(WaitFor([&] { return fired.load(); }));
  auto url = UniqueUrl("rg32");
  Publisher pub(url);
  Subscriber sub(url);
  EXPECT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
}

// RG-33 (fourth review, M-a) A publisher whose discovery fails for lack of
// memory is found by a rescan, whichever way it was being discovered.
TEST(ShmRegressionTest, DiscoveryOutOfMemoryRescans) {
  struct Restore {
    ~Restore() {
      fault::offer_bad_alloc.store(0);
      detail::RuntimeImpl::Instance()->Call([] { test::FailAllocations(0); });
    }
  } restore;
  {  // inotify: the subscriber is first, the publisher's IN_MOVED_TO fails
    auto url = UniqueUrl("rg33a");
    Subscriber sub(url);
    fault::offer_bad_alloc.store(1);
    Publisher pub(url);
    ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
    EXPECT_EQ(fault::offer_bad_alloc.load(), 0u);
  }
  {  // scan: the publisher is first, the new subscriber's scan fails
    auto url = UniqueUrl("rg33b");
    Publisher pub(url);
    fault::offer_bad_alloc.store(1);
    Subscriber sub(url);
    ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
    EXPECT_EQ(fault::offer_bad_alloc.load(), 0u);
  }
  {  // no allocation works on the control thread for a while: the rescan
     // itself has to wait, and backs off without needing memory
    auto url = UniqueUrl("rg33c");
    Subscriber sub(url);
    auto* rt = detail::RuntimeImpl::Instance();
    rt->Call([] { test::FailAllocations(INT_MAX); });
    RawPublisher raw;
    ASSERT_TRUE(raw.Listen(url));
    std::this_thread::sleep_for(50ms);
    rt->Call([] { test::FailAllocations(0); });
    ASSERT_TRUE(raw.Handshake());
    ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 1; }));
  }
}

// RG-34 (fourth review, M-b) A Welcome that arrives after Subscriber::Shutdown()
// looked at its connections: nothing was delivered, nothing is held, and
// the hang-up has to read as an orderly close.
RawPublisher* g_rg34_raw = nullptr;
TEST(ShmRegressionTest, WelcomeDuringShutdownClosesInOrder) {
  auto url = UniqueUrl("rg34");
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url));
  auto sub = std::make_unique<Subscriber>(url);
  ASSERT_TRUE(raw.AcceptHello());
  g_rg34_raw = &raw;
  fault::subscriber_shutdown_pause.store(+[] {
    // Past the point where Shutdown() used to set the flag, control thread free.
    ASSERT_TRUE(g_rg34_raw->SendWelcome());
    ASSERT_TRUE(WaitFor([] { return g_rg34_raw->Readable() || g_rg34_raw->hdr->closed.load() != 0; }));
  });
  sub.reset();
  fault::subscriber_shutdown_pause.store(nullptr);
  EXPECT_NE(raw.hdr->closed.load(), 0u);
  EXPECT_EQ(raw.Expect(MsgType::kConnAck, 1), 0);
}

// RG-35 (fourth review, M-c) A subscriber that gives a publisher up while it
// holds a sample (here: a segment it cannot map) must say so at once. The
// publisher quarantines what it holds, a lossless publish does not wait
// for a ring nobody reads, and the subscriber can reconnect beside the old
// connection; the last release is the late orderly close.
TEST(ShmRegressionTest, OrphanWithHeldSampleHangsUp) {
  struct Restore {
    ~Restore() { fault::map_segment_fail.store(0); }
  } restore;
  auto url = UniqueUrl("rg35");
  PublisherOptions po;
  po.too_slow = TooSlowPolicy::kWaitForConsumer;
  po.block_timeout = 3s;
  SubscriberOptions so;
  so.queue_capacity = 2;
  so.queue_full = QueueFullPolicy::kBlockProducer;
  Publisher pub(url, po);
  Subscriber sub(url, so);
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto held = sub.Take();
  ASSERT_TRUE(held.ok()) << ErrorName(held.error());

  fault::map_segment_fail.store(1);
  auto big = pub.Loan(4096);  // the SegmentAdd the subscriber cannot map
  ASSERT_TRUE(big.ok()) << ErrorName(big.error());
  pub.Release(*big);
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 0; }));
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 0; }, 2s));
  EXPECT_EQ(pub.Stats().quarantined_chunks, 1u);

  const auto t0 = std::chrono::steady_clock::now();
  for (uint64_t s = 2; s <= 6; ++s) PublishSeq(pub, s);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
  EXPECT_EQ(pub.Stats().block_timeouts, 0u);
  EXPECT_EQ(pub.Stats().quarantined_chunks, 1u);

  // Any scan of this URL offers the publisher again; known_paths_ forgot it
  // with the hang-up although the old connection is still there.
  Subscriber other(url, so);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 2 && sub.Stats().connections == 1; }));
  PublishSeq(pub, 7);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto fresh = sub.Take();
  ASSERT_TRUE(fresh.ok()) << ErrorName(fresh.error());
  EXPECT_EQ(CheckPattern(*fresh, 64), 7u);
  EXPECT_EQ(CheckPattern(*held, 64), 1u);
  sub.Release(*fresh);

  sub.Release(*held);
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().quarantined_chunks == 0; }));
  EXPECT_EQ(sub.Stats().connections, 1u);
  EXPECT_EQ(pub.Stats().connections, 2u);
}

// RG-36 (fourth review, L-2) ENOBUFS/ENOMEM from sendmsg with a socket that
// polls writable: EPOLLOUT would spin the control thread. The publisher
// retries on a timer and nothing is lost.
TEST(ShmRegressionTest, PublisherBacksOffWhileOutOfSocketBuffers) {
  struct Restore {
    ~Restore() { fault::send_no_buffers.store(0); }
  } restore;
  auto url = UniqueUrl("rg36");
  Publisher pub(url);
  Subscriber sub(url);
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers() && sub.Stats().connections == 1; }));
  constexpr uint32_t kMany = 1u << 30;
  const auto t0 = std::chrono::steady_clock::now();
  fault::send_no_buffers.store(kMany);
  auto big = pub.Loan(4096);  // a SegmentAdd that cannot be sent
  ASSERT_TRUE(big.ok()) << ErrorName(big.error());
  std::this_thread::sleep_for(100ms);
  const uint32_t attempts = kMany - fault::send_no_buffers.exchange(0);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
  EXPECT_GE(attempts, 2u);  // the Send() and at least one retry
  EXPECT_LE(attempts, static_cast<uint64_t>(elapsed.count()) / kNoBuffersRetryMs + 3);
  FillPattern(*big, 4096, 5);
  ASSERT_TRUE(pub.Publish(*big));
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  auto t = sub.Take();
  ASSERT_TRUE(t.ok()) << ErrorName(t.error());
  EXPECT_EQ(CheckPattern(*t, 4096), 5u);
  sub.Release(*t);
  EXPECT_EQ(pub.Stats().connections, 1u);
  EXPECT_EQ(pub.Stats().quarantined, 0u);
}

// RG-37 (fourth review, L-2) The same for the subscriber's acks.
TEST(ShmRegressionTest, SubscriberBacksOffWhileOutOfSocketBuffers) {
  struct Restore {
    ~Restore() { fault::send_no_buffers.store(0); }
  } restore;
  auto url = UniqueUrl("rg37");
  RawPublisher raw;
  ASSERT_TRUE(raw.Listen(url));
  Subscriber sub(url);
  ASSERT_TRUE(raw.Handshake());
  constexpr uint32_t kMany = 1u << 30;
  const auto t0 = std::chrono::steady_clock::now();
  {
    ControlThreadStall stall;  // our own send must not hit the fault
    ASSERT_TRUE(raw.SendSegmentAdd(1));
    fault::send_no_buffers.store(kMany);
  }
  std::this_thread::sleep_for(100ms);
  const uint32_t attempts = kMany - fault::send_no_buffers.exchange(0);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
  EXPECT_GE(attempts, 2u);
  EXPECT_LE(attempts, static_cast<uint64_t>(elapsed.count()) / kNoBuffersRetryMs + 3);
  EXPECT_EQ(raw.Expect(MsgType::kSegmentAck, 1), 1);
  EXPECT_EQ(sub.Stats().connections, 1u);
}

// RG-38 (fourth review, M-d) Group mode as one user: a setgid directory of the
// group, sockets in that group with mode 0660. The group may be any of
// ours, not just the effective one, and bits a directory of ours should
// not have are removed.
TEST(ShmRegressionTest, GroupModeSetsDirectoryAndSocketAccess) {
  std::string root = test::TestRoot() + "/rg38_XXXXXX";  // fresh for every --gtest_repeat round
  ASSERT_NE(::mkdtemp(root.data()), nullptr) << std::strerror(errno);
  gid_t gid = ::getegid();
  gid_t groups[64];
  int n = ::getgroups(64, groups);
  for (int i = 0; i < n; ++i) {
    if (groups[i] != gid) {
      gid = groups[i];
      break;
    }
  }
  RecordProperty("supplementary_group", gid != ::getegid() ? "yes" : "no");
  const std::string g = std::to_string(gid);
  const std::string dir = root + "/ibmw_shm.g" + g + ".dom";
  ASSERT_EQ(::mkdir(dir.c_str(), 0777), 0);
  ASSERT_EQ(::chmod(dir.c_str(), 0777), 0);
  auto r = Probe({UniqueUrl("rg38")}, {"IBMW_SHM_ROOT=" + root, "IBMW_SHM_DOMAIN=dom", "IBMW_SHM_GROUP=" + g});
  EXPECT_EQ(r.dir, dir);
  EXPECT_EQ(r.pub, "ok");
  EXPECT_EQ(r.sub, "ok");
  EXPECT_EQ(r.sock, "660 " + g);
  struct stat st {};
  ASSERT_EQ(::stat(dir.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 07777, 02770u);
  EXPECT_EQ(st.st_gid, gid);
}

// RG-39 (fourth review, M-d) Group mode between real users: publisher and
// subscriber of different uids meet, the subscriber being in the group only
// as a supplementary group where SO_PEERGROUPS can tell. Everyone else is
// kept out, and a directory that is open to others or not writable by the
// group is refused. Root only; run_shm_ohos.sh fails if this was skipped.
TEST(ShmRegressionTest, CrossUidGroupMode) {
  if (::geteuid() != 0) GTEST_SKIP() << "requires root: forks peers that call setresuid/setresgid";
  constexpr gid_t kGroup = 4300;
  const std::string group = std::to_string(kGroup);
  ASSERT_EQ(::chmod(test::TestRoot().c_str(), 0711), 0);
  std::string root = test::TestRoot() + "/rg39_XXXXXX";
  ASSERT_NE(::mkdtemp(root.data()), nullptr) << std::strerror(errno);
  ASSERT_EQ(::chmod(root.c_str(), 01777), 0);
  auto env = [&](const std::string& domain) {
    return std::vector<std::string>{"IBMW_SHM_ROOT=" + root, "IBMW_SHM_DOMAIN=" + domain, "IBMW_SHM_GROUP=" + group};
  };
  const bool peer_groups = PeerGroupsSupported();
  RecordProperty("so_peergroups", peer_groups ? "yes" : "no");
  const std::vector<std::string> as_pub{"as", "4201", group, "-"};
  // Without SO_PEERGROUPS membership must be the effective gid (docs, 3.7).
  const std::vector<std::string> as_sub = peer_groups ? std::vector<std::string>{"as", "4202", "4302", group}
                                                      : std::vector<std::string>{"as", "4202", group, "-"};
  const std::vector<std::string> as_other{"as", "4203", "4303", "-"};
  auto args = [](std::vector<std::string> as, const std::vector<std::string>& rest) {
    as.insert(as.end(), rest.begin(), rest.end());
    return as;
  };

  const auto url = UniqueUrl("rg39");
  auto pub = test::SpawnPeer(args(as_pub, {"pub", url, "10", "1", "64"}), env("dom"));
  struct Reap {
    test::Peer& p;
    ~Reap() {
      if (p.pid > 0) {
        test::KillPeer(p, SIGKILL);
        test::WaitPeer(p);
      }
    }
  } reap{pub};
  ASSERT_EQ(test::ReadLine(pub), "ready");

  const std::string dir = root + "/ibmw_shm.g" + group + ".dom";
  struct stat st {};
  ASSERT_EQ(::stat(dir.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 07777, 02770u);
  EXPECT_EQ(st.st_uid, 4201u);
  EXPECT_EQ(st.st_gid, kGroup);
  std::string sock;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (auto* de = ::readdir(d)) {
      if (de->d_name[0] != '.') sock = dir + "/" + de->d_name;
    }
    ::closedir(d);
  }
  ASSERT_FALSE(sock.empty());
  ASSERT_EQ(::stat(sock.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0660u);
  EXPECT_EQ(st.st_gid, kGroup);

  // Not in the group: the runtime refuses the directory.
  auto r = Probe({UniqueUrl("rg39")}, env("dom"), as_other);
  EXPECT_EQ(r.pub, "permission");
  EXPECT_EQ(r.sub, "permission");

  auto sub = test::SpawnPeer(args(as_sub, {"sub", url, "10", "1", "64"}), env("dom"));
  ASSERT_EQ(test::ReadLine(sub), "ready");
  EXPECT_EQ(test::ReadLine(pub), "done 10");
  EXPECT_EQ(test::ReadLine(sub), "done 10");
  EXPECT_EQ(test::WaitPeer(sub), 0);

  // Directories prepared by someone else: open to others, or the group
  // cannot write, or another group.
  struct Foreign {
    const char* domain;
    gid_t gid;
    mode_t mode;
  };
  for (const Foreign& f : {Foreign{"open", kGroup, 0777}, Foreign{"ro", kGroup, 0750}, Foreign{"other", 4303, 0770}}) {
    const std::string d = root + "/ibmw_shm.g" + group + "." + f.domain;
    ASSERT_EQ(::mkdir(d.c_str(), 0700), 0);
    ASSERT_EQ(::chown(d.c_str(), 4201, f.gid), 0);
    ASSERT_EQ(::chmod(d.c_str(), f.mode), 0);
    r = Probe({UniqueUrl("rg39")}, env(f.domain), as_sub);
    EXPECT_EQ(r.pub, "permission") << f.domain;
    EXPECT_EQ(r.sub, "permission") << f.domain;
  }
}

// RG-40 (fourth review, M-a, L-1) Shutdown is noexcept: nothing on the way may
// depend on an allocation succeeding, and connections must still close in
// order then.
TEST(ShmRegressionTest, ShutdownSurvivesOutOfMemory) {
  auto url = UniqueUrl("rg40");
  auto pub = std::make_unique<Publisher>(url);
  auto sub = std::make_unique<Subscriber>(url);
  auto gone = std::make_unique<Subscriber>(url);
  ASSERT_TRUE(WaitFor([&] { return pub->Stats().connections == 2 && sub->Stats().connections == 1; }));
  PublishSeq(*pub, 1);
  ASSERT_TRUE(WaitFor([&] { return gone->HasData(); }));
  ASSERT_TRUE(gone->Take().ok());

  test::FailAllocations(8);
  gone.reset();
  test::FailAllocations(0);
  ASSERT_TRUE(WaitFor([&] { return pub->Stats().connections == 1; }));
  EXPECT_EQ(pub->Stats().quarantined, 0u);

  test::FailAllocations(8);
  pub.reset();
  test::FailAllocations(0);
  EXPECT_TRUE(WaitFor([&] { return sub->Stats().connections == 0; }));
}

// RG-41 (fourth review, M-a) Runtime::Shutdown() is noexcept too. A queued stop
// command could fail to allocate only at a block boundary of the command
// deque, hence every position in a block.
TEST(ShmRegressionTest, RuntimeShutdownNeedsNoMemory) {
  for (int n = 0; n < 40; ++n) {
    auto peer = test::SpawnPeer({"runtime_shutdown_oom", UniqueUrl("rg41"), std::to_string(n)});
    EXPECT_EQ(test::ReadLine(peer), "done") << n;
    EXPECT_EQ(test::WaitPeer(peer), 0) << n;
  }
}

// RG-42 (fourth review, L-4) Re-creating the service directory keeps being
// retried when it fails and memory is short: the retry is no allocated timer
// that could silently not exist.
TEST(ShmRegressionTest, RewatchRetriesWithoutMemory) {
  auto url = UniqueUrl("rg42");
  Subscriber sub(url);
  const std::string dir = Runtime::ServiceDirectory();
  auto* rt = detail::RuntimeImpl::Instance();
  struct Restore {
    detail::RuntimeImpl* rt;
    ~Restore() {
      rt->Call([] { test::FailAllocations(0); });
    }
  } restore{rt};
  rt->Call([] { test::FailAllocations(INT_MAX); });
  {
    ControlThreadStall stall;  // the removal is only seen once the file is there
    ASSERT_TRUE(RemoveServiceDirectory()) << std::strerror(errno);
    int fd = ::open(dir.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);  // in the way: mkdir fails
    ASSERT_GE(fd, 0) << std::strerror(errno);
    ::close(fd);
  }
  std::this_thread::sleep_for(100ms);
  struct stat st {};
  ASSERT_EQ(::stat(dir.c_str(), &st), 0);
  EXPECT_TRUE(S_ISREG(st.st_mode));
  rt->Call([] { test::FailAllocations(0); });
  ASSERT_EQ(::unlink(dir.c_str()), 0);
  ASSERT_TRUE(WaitFor([&] { return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }));
  // Watching again: found through inotify or the rescan after the re-watch.
  RawPublisher raw;
  ASSERT_TRUE(WaitFor([&] { return raw.Listen(url); }));
  ASSERT_TRUE(raw.Handshake());
  ASSERT_TRUE(WaitFor([&] { return sub.Stats().connections == 1; }));
}

}  // namespace
}  // namespace ibmw::selfdev::shm
