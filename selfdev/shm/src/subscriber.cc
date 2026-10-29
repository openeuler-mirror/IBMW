// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

#include "impl.h"

namespace ibmw::selfdev::shm {

Subscriber::Subscriber(std::string_view url, const SubscriberOptions& options) {
  auto r = detail::SubscriberImpl::Create(url, options);
  if (r) {
    impl_ = std::move(r.value());
    impl_->owner = this;
  } else {
    init_error_ = r.error();
  }
}

Subscriber::~Subscriber() {
  if (!impl_) return;
  if (auto l = impl_->AttachedListener()) l->Detach(impl_.get());
  impl_->Shutdown();
}

bool Subscriber::Valid() const noexcept { return impl_ != nullptr; }
Error Subscriber::InitError() const noexcept { return init_error_; }
bool Subscriber::HasData() const noexcept { return impl_ && impl_->HasData(); }

Result<const void*> Subscriber::Take() noexcept {
  if (!impl_) return Error::kNotConnected;
  return impl_->Take();
}

void Subscriber::Release(const void* user_payload) noexcept {
  if (impl_ && user_payload != nullptr) impl_->Release(user_payload);
}

SubscriberStats Subscriber::Stats() const noexcept { return impl_ ? impl_->Stats() : SubscriberStats{}; }

namespace detail {

namespace {

void Log(const char* what, const std::string& url) {
  std::fprintf(stderr, "[ibmw_shm] subscriber %s: %s\n", url.c_str(), what);
}

}  // namespace

Result<std::shared_ptr<SubscriberImpl>> SubscriberImpl::Create(std::string_view url,
                                                               const SubscriberOptions& options) {
  if (url.empty() || url.size() > kMaxUrlLength) return Error::kInvalidArg;
  auto* rt = RuntimeImpl::Instance();
  if (rt == nullptr) return Error::kSystem;
  std::shared_ptr<SubscriberImpl> self(new SubscriberImpl());
  self->rt_ = rt;
  self->url_ = std::string(url);
  self->hash_ = Hash64(url);
  self->options_ = options;
  self->options_.queue_capacity = std::clamp<uint32_t>(options.queue_capacity, 1, kMaxSubscriberQueueCapacity);
  self->options_.max_held = std::clamp<uint32_t>(options.max_held, 1, kMaxChunksHeldPerSubscriber);
  // Register before scanning so a publisher appearing in between is offered
  // through inotify; duplicates are filtered by known_paths_.
  rt->RegisterSubscriber(self->hash_, self);
  rt->Call([&] { rt->ScanFor(self->hash_); });
  return self;
}

SubscriberImpl::~SubscriberImpl() = default;

void SubscriberImpl::Shutdown() noexcept {
  rt_->UnregisterSubscriber(hash_, this);
  std::vector<std::shared_ptr<SubConn>> conns;
  {
    std::lock_guard<std::mutex> lock(mu_);
    shutting_down_ = true;
    conns = conns_;
  }
  rt_->Call([&] {
    for (auto& c : conns) {
      rt_->RemoveFd(c->sock.Get());
      ::shutdown(c->sock.Get(), SHUT_RDWR);
    }
  });
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& c : conns_) {
    if (c->hdr != nullptr) c->hdr->closed.store(1, std::memory_order_release);
  }
  held_.clear();
  conns_.clear();
}

// ------------------------------------------------------- control thread

void SubscriberImpl::ConnectTo(const std::string& path) noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_ || known_paths_.count(path) != 0) return;
  }
  struct sockaddr_un addr {};
  if (path.size() >= sizeof(addr.sun_path)) return;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  struct stat before {};
  if (::lstat(path.c_str(), &before) != 0) return;

  UniqueFd sock(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!sock.Valid()) return;
  if (::connect(sock.Get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    if (errno == ECONNREFUSED) {
      // Publisher died without unlinking. The file name carries a random
      // nonce, so only remove it if it is still the same inode.
      struct stat now {};
      if (::lstat(path.c_str(), &now) == 0 && now.st_ino == before.st_ino && now.st_dev == before.st_dev) {
        ::unlink(path.c_str());
      }
    } else {
      Log("connect failed", url_);
    }
    return;
  }
  PeerCredentials peer;
  if (!GetPeerCredentials(sock.Get(), &peer) || !rt_->PeerAllowed(peer)) {
    Log("publisher credentials not allowed", url_);
    return;
  }
  HelloMsg hello{};
  hello.h = {kMsgMagic, MsgType::kHello};
  hello.abi = kAbiVersion;
  hello.queue_capacity = options_.queue_capacity;
  hello.queue_full = static_cast<uint32_t>(options_.queue_full);
  hello.max_held = options_.max_held;
  hello.url_length = static_cast<uint32_t>(url_.size());
  std::memcpy(hello.url, url_.data(), url_.size());
  int nfd = rt_->NotifyFd();
  if (!SendMessage(sock.Get(), &hello, offsetof(HelloMsg, url) + url_.size(), &nfd, 1)) return;

  auto conn = std::make_shared<SubConn>();
  conn->sub = weak_from_this();
  conn->path = path;
  conn->sock = std::move(sock);
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutting_down_) return;
    known_paths_.insert(path);
    conns_.push_back(conn);
  }
  if (!rt_->AddFd(conn->sock.Get(), EPOLLIN | EPOLLRDHUP, conn)) {
    std::lock_guard<std::mutex> lock(mu_);
    known_paths_.erase(path);
    RemoveConnLocked(conn.get());
  }
}

void SubConn::OnEvent(uint32_t events) noexcept {
  auto self = shared_from_this();
  auto s = sub.lock();
  if (!s) return;
  if ((events & EPOLLIN) != 0) {
    alignas(8) uint8_t buf[kMaxMsgSize];
    for (;;) {
      std::vector<UniqueFd> fds;
      ssize_t n = ReceiveMessage(sock.Get(), buf, sizeof(buf), &fds);
      if (n < 0 && errno == EAGAIN) break;
      if (n <= 0) {
        s->HandleHangup(self);
        return;
      }
      s->HandleMessage(self, buf, static_cast<size_t>(n), fds);
    }
  }
  if ((events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) != 0) s->HandleHangup(self);
}

void SubscriberImpl::HandleMessage(const std::shared_ptr<SubConn>& conn, const uint8_t* data, size_t len,
                                   std::vector<UniqueFd>& fds) noexcept {
  MsgHeader mh{};
  if (len < sizeof(mh)) return HandleHangup(conn);
  std::memcpy(&mh, data, sizeof(mh));
  if (mh.magic != kMsgMagic) return HandleHangup(conn);

  if (mh.type == MsgType::kWelcome) {
    WelcomeMsg wm{};
    if (len < sizeof(wm) || fds.size() != 1) return HandleHangup(conn);
    std::memcpy(&wm, data, sizeof(wm));
    if (wm.abi != kAbiVersion) {
      Log("publisher ABI mismatch", url_);
      return HandleHangup(conn);
    }
    if (wm.queue_capacity == 0 || wm.queue_capacity > kMaxSubscriberQueueCapacity ||
        wm.delivery_slots != wm.queue_capacity + 1 || wm.return_slots < 2 ||
        wm.return_slots > kMaxSubscriberQueueCapacity + kMaxChunksHeldPerSubscriber + 1 ||
        wm.segment_size != ConnSegmentSize(wm.delivery_slots, wm.return_slots)) {
      return HandleHangup(conn);
    }
    auto m = MapSegment(fds[0].Get(), wm.segment_size, true);
    if (!m) return HandleHangup(conn);
    auto* h = reinterpret_cast<ConnHeader*>(m->Data());
    if (h->magic != kConnMagic) return HandleHangup(conn);
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (conn->state != SubConn::State::kAwaitWelcome) return;
      conn->conn_map = std::move(m.value());
      conn->hdr = h;
      conn->capacity = wm.queue_capacity;
      conn->delivery_slots = wm.delivery_slots;
      conn->return_slots = wm.return_slots;
      conn->return_write = h->return_write.load(std::memory_order_relaxed);
      h->notify_slot.store(notify_slot_.load(std::memory_order_relaxed), std::memory_order_seq_cst);
      conn->state = SubConn::State::kActive;
    }
    MsgHeader ack{kMsgMagic, MsgType::kConnAck};
    if (!SendMessage(conn->sock.Get(), &ack, sizeof(ack), nullptr, 0)) HandleHangup(conn);
    return;
  }

  if (mh.type == MsgType::kSegmentAdd) {
    SegmentAddMsg sm{};
    if (len < sizeof(sm) || fds.size() != 1) return HandleHangup(conn);
    std::memcpy(&sm, data, sizeof(sm));
    if (sm.segment_id >= kMaxSegmentsPerPublisher || sm.chunk_count == 0 || sm.chunk_count > kMaxChunksPerSegment ||
        sm.chunk_stride < kChunkHeaderSize || sm.chunk_stride % 64 != 0 ||
        sm.chunk_stride > (1ull << kMaxClassShift) + kChunkHeaderSize ||
        sm.segment_size != sm.chunk_stride * sm.chunk_count) {
      return HandleHangup(conn);
    }
    // Data segments are mapped read-only; with F_SEAL_FUTURE_WRITE the
    // kernel would refuse a writable mapping anyway.
    auto m = MapSegment(fds[0].Get(), sm.segment_size, false);
    if (!m) return HandleHangup(conn);
    bool duplicate = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      // Re-sending a segment id would unmap chunks the user may hold.
      duplicate = conn->segments.count(sm.segment_id) != 0;
      if (duplicate) {
        MarkCorruptLocked(*conn);
      } else {
        SubSegment seg;
        seg.map = std::move(m.value());
        seg.stride = sm.chunk_stride;
        seg.count = sm.chunk_count;
        conn->segments.emplace(sm.segment_id, std::move(seg));
      }
    }
    if (duplicate) return HandleHangup(conn);
    SegmentAckMsg ack{{kMsgMagic, MsgType::kSegmentAck}, sm.segment_id};
    if (!SendMessage(conn->sock.Get(), &ack, sizeof(ack), nullptr, 0)) HandleHangup(conn);
    return;
  }

  if (mh.type == MsgType::kReject) {
    Log("rejected by publisher", url_);
  }
  HandleHangup(conn);
}

void SubscriberImpl::HandleHangup(const std::shared_ptr<SubConn>& conn) noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    known_paths_.erase(conn->path);
    if (conn->state != SubConn::State::kOrphan) {
      // A publisher that shut down cleanly sets `closed` first and never
      // reuses its chunks again, so what is queued can still be taken.
      conn->draining = conn->state == SubConn::State::kActive && conn->hdr != nullptr &&
                       conn->hdr->closed.load(std::memory_order_acquire) != 0;
      conn->state = SubConn::State::kOrphan;
    }
    // Taken chunks stay readable (mappings live in the connection) until
    // released; then the connection goes away.
    PruneOrphansLocked();
  }
  rt_->RemoveFd(conn->sock.Get());
}

void SubscriberImpl::RemoveConnLocked(const SubConn* conn) noexcept {
  auto it = std::find_if(conns_.begin(), conns_.end(), [conn](auto& c) { return c.get() == conn; });
  if (it == conns_.end()) return;
  // Keeps Poll()'s version monotonic once the connection is gone.
  if (conn->hdr != nullptr) removed_version_ += conn->hdr->delivery_write.load(std::memory_order_relaxed);
  conns_.erase(it);
}

void SubscriberImpl::PruneOrphansLocked() noexcept {
  for (size_t i = conns_.size(); i-- > 0;) {
    SubConn& c = *conns_[i];
    if (c.state == SubConn::State::kOrphan && !c.draining && c.held == 0) RemoveConnLocked(&c);
  }
}

// Data-path counterpart of the publisher's MarkCorruptLocked: stop reading
// from the connection; the control thread sees the hang-up and cleans up.
void SubscriberImpl::MarkCorruptLocked(SubConn& conn) noexcept {
  if (conn.state != SubConn::State::kOrphan) Log("connection corrupted, dropping publisher", url_);
  ++stats_.corrupted_connections;
  conn.state = SubConn::State::kOrphan;
  conn.draining = false;
  ::shutdown(conn.sock.Get(), SHUT_RDWR);
}

// --------------------------------------------------------------- data path

bool SubscriberImpl::HasData() const noexcept {
  uint64_t v;
  return Poll(&v);
}

bool SubscriberImpl::Poll(uint64_t* version) const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  bool has = false;
  uint64_t v = removed_version_;
  for (auto& c : conns_) {
    if (c->hdr == nullptr) continue;
    uint64_t w = c->hdr->delivery_write.load(std::memory_order_acquire);
    v += w;
    if (c->state == SubConn::State::kActive || c->draining) {
      has = has || c->hdr->delivery_read.load(std::memory_order_acquire) < w;
    }
  }
  *version = v;
  return has;
}

void SubscriberImpl::SignalSpace(SubConn& conn) noexcept {
  // Pairs with the producer's fence before re-checking a full ring.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  ConnHeader* h = conn.hdr;
  if (h->space_sleeping.load(std::memory_order_relaxed) != 0 &&
      h->space_sleeping.exchange(0, std::memory_order_acq_rel) != 0) {
    h->space_epoch.fetch_add(1, std::memory_order_release);
    FutexWake(&h->space_epoch, INT_MAX);
  }
}

Result<const void*> SubscriberImpl::Take() noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  if (shutting_down_) return Error::kShuttingDown;
  if (held_.size() >= options_.max_held) return Error::kTooManyHeld;
  PruneOrphansLocked();
  const size_t n = conns_.size();
  for (size_t k = 0; k < n; ++k) {
    size_t ci = (round_robin_ + k) % n;
    SubConn& c = *conns_[ci];
    const bool active = c.state == SubConn::State::kActive;
    if (!active && !c.draining) continue;
    ConnHeader* h = c.hdr;
    auto* slots = DeliverySlots(h);
    // Bounded: the ring indices are publisher-writable.
    for (uint32_t spin = 0; spin < 4 * c.capacity + 4; ++spin) {
      uint64_t r = h->delivery_read.load(std::memory_order_acquire);
      uint64_t w = h->delivery_write.load(std::memory_order_acquire);
      if (r >= w) {
        c.draining = false;  // an orphan's ring never refills
        break;
      }
      if (w - r > c.capacity) {
        // r was loaded before w, and an evicting publisher may have advanced
        // both in between. Only a read index loaded after w is bounded by it.
        uint64_t r2 = h->delivery_read.load(std::memory_order_acquire);
        if (r2 >= w || w - r2 <= c.capacity) continue;
        MarkCorruptLocked(c);
        break;
      }
      uint64_t v = slots[r % c.delivery_slots].load(std::memory_order_acquire);
      // The CAS decides ownership against a concurrent eviction.
      if (!h->delivery_read.compare_exchange_strong(r, r + 1, std::memory_order_acq_rel)) continue;
      if (active && options_.queue_full == QueueFullPolicy::kBlockProducer) SignalSpace(c);
      auto seg = c.segments.find(HandleSegment(v));
      uint32_t idx = HandleIndex(v);
      if (seg == c.segments.end() || idx >= seg->second.count) {
        PushReturnLocked(c, v);  // the publisher counts it as a bad return
        continue;
      }
      const uint8_t* base = seg->second.map.Data() + idx * seg->second.stride;
      auto* ch = reinterpret_cast<const ChunkHeader*>(base);
      // Read once: the header is in publisher-writable memory.
      uint32_t offset = __atomic_load_n(&ch->payload_offset, __ATOMIC_RELAXED);
      uint32_t size = __atomic_load_n(&ch->payload_size, __ATOMIC_RELAXED);
      if (offset < kChunkHeaderSize || static_cast<uint64_t>(offset) + size > seg->second.stride) {
        PushReturnLocked(c, v);
        continue;
      }
      const void* payload = base + offset;
      if (held_.count(payload) != 0) {  // delivered twice while held
        PushReturnLocked(c, v);
        continue;
      }
      held_.emplace(payload, Held{conns_[ci], v});
      ++c.held;
      ++stats_.takes;
      round_robin_ = (ci + 1) % n;
      return payload;
    }
  }
  return Error::kNoData;
}

void SubscriberImpl::PushReturnLocked(SubConn& conn, uint64_t handle) noexcept {
  if (conn.state != SubConn::State::kActive) return;  // the publisher is gone
  ConnHeader* h = conn.hdr;
  const uint64_t r = h->return_read.load(std::memory_order_acquire);
  auto* slots = ReturnSlots(h, conn.delivery_slots);
  auto push = [&](uint64_t v) {
    if (conn.return_write - r >= conn.return_slots) return false;
    slots[conn.return_write % conn.return_slots].store(v, std::memory_order_relaxed);
    ++conn.return_write;
    return true;
  };
  while (!conn.return_backlog.empty() && push(conn.return_backlog.front())) conn.return_backlog.pop_front();
  if (!conn.return_backlog.empty() || !push(handle)) {
    // Cannot happen with a well-behaved publisher, which drains before every
    // delivery. Park the handle so the chunk is not lost.
    ++stats_.return_ring_full;
    if (conn.return_backlog.size() < kMaxReturnBacklog) conn.return_backlog.push_back(handle);
  }
  h->return_write.store(conn.return_write, std::memory_order_release);
}

void SubscriberImpl::Release(const void* user_payload) noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = held_.find(user_payload);
  if (it == held_.end()) return;
  auto conn = std::move(it->second.conn);
  uint64_t handle = it->second.handle;
  held_.erase(it);
  --conn->held;
  ++stats_.releases;
  PushReturnLocked(*conn, handle);
  if (conn->state == SubConn::State::kOrphan) PruneOrphansLocked();
}

SubscriberStats SubscriberImpl::Stats() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  SubscriberStats s = stats_;
  s.connections = 0;
  for (auto& c : conns_) s.connections += c->state == SubConn::State::kActive ? 1 : 0;
  s.held = static_cast<uint32_t>(held_.size());
  return s;
}

std::shared_ptr<ListenerImpl> SubscriberImpl::AttachedListener() const noexcept {
  std::lock_guard<std::mutex> lock(listener_mu_);
  return listener_.lock();
}

bool SubscriberImpl::ClaimListener(const std::shared_ptr<ListenerImpl>& listener) noexcept {
  std::lock_guard<std::mutex> lock(listener_mu_);
  auto current = listener_.lock();
  if (current && current != listener) return false;
  listener_ = listener;
  return true;
}

void SubscriberImpl::ClearListener(const ListenerImpl* listener) noexcept {
  std::lock_guard<std::mutex> lock(listener_mu_);
  if (listener_.lock().get() == listener) listener_.reset();
}

void SubscriberImpl::SetNotifySlot(uint32_t slot) noexcept {
  notify_slot_.store(slot, std::memory_order_seq_cst);
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& c : conns_) {
    if (c->hdr != nullptr) c->hdr->notify_slot.store(slot, std::memory_order_seq_cst);
  }
}

}  // namespace detail
}  // namespace ibmw::selfdev::shm
