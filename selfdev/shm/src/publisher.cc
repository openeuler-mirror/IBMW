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

// ------------------------------------------------------------------ public

Publisher::Publisher(std::string_view url, const PublisherOptions& options) {
  auto r = detail::PublisherImpl::Create(url, options);
  if (r) {
    impl_ = std::move(r.value());
  } else {
    init_error_ = r.error();
  }
}

Publisher::~Publisher() {
  if (impl_) impl_->Shutdown();
}

bool Publisher::Valid() const noexcept { return impl_ != nullptr; }
Error Publisher::InitError() const noexcept { return init_error_; }

Result<void*> Publisher::Loan(uint32_t payload_size, uint32_t alignment) noexcept {
  if (!impl_) return Error::kNotConnected;
  return impl_->Loan(payload_size, alignment);
}

bool Publisher::Publish(void* user_payload) noexcept {
  return impl_ && user_payload != nullptr && impl_->Publish(user_payload);
}

void Publisher::StopBlocking() noexcept {
  if (impl_) impl_->StopBlocking();
}

bool Publisher::Release(void* user_payload) noexcept {
  return impl_ && user_payload != nullptr && impl_->Release(user_payload);
}

bool Publisher::HasSubscribers() const noexcept { return impl_ && impl_->HasSubscribers(); }

PublisherStats Publisher::Stats() const noexcept { return impl_ ? impl_->Stats() : PublisherStats{}; }

ChunkView ChunkView::FromUserPayload(const void* user_payload) noexcept {
  auto* p = static_cast<const uint8_t*>(user_payload);
  uint32_t back;
  std::memcpy(&back, p - sizeof(uint32_t), sizeof(back));
  return ChunkView(reinterpret_cast<const ChunkHeader*>(p - back));
}

bool ChunkView::Valid() const noexcept {
  return header_ != nullptr && header_->magic == kChunkMagic && header_->abi == kAbiVersion;
}

namespace detail {

namespace {

constexpr uint32_t kNumClasses = kMaxClassShift + 1;

uint32_t ClassShiftFor(uint64_t bytes) noexcept {
  uint32_t shift = kMinClassShift;
  while (shift <= kMaxClassShift && (1ull << shift) < bytes) ++shift;
  return shift;
}

uint32_t InitialChunks(uint32_t class_shift) noexcept {
  if (class_shift < 20) return 4;  // < 1 MB
  if (class_shift < 24) return 2;  // < 16 MB
  return 1;
}

bool IsPowerOfTwo(uint32_t v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

void Log(const char* what, const std::string& url) {
  std::fprintf(stderr, "[ibmw_shm] publisher %s: %s\n", url.c_str(), what);
}

}  // namespace

// --------------------------------------------------------------- creation

Result<std::shared_ptr<PublisherImpl>> PublisherImpl::Create(std::string_view url,
                                                             const PublisherOptions& options) {
  if (url.empty() || url.size() > kMaxUrlLength) return Error::kInvalidArg;
  auto* rt = RuntimeImpl::Instance();
  if (rt == nullptr) return Error::kSystem;

  std::shared_ptr<PublisherImpl> self(new PublisherImpl());
  self->rt_ = rt;
  self->url_ = std::string(url);
  self->hash_ = Hash64(url);
  self->options_ = options;
  self->free_.resize(kNumClasses);
  self->class_chunks_.assign(kNumClasses, 0);

  // Eager pools first, so subscribers see them in the first handshake.
  for (const auto& pc : options.pools) {
    uint32_t shift = ClassShiftFor(std::max<uint32_t>(pc.payload_size, 1));
    if (shift > kMaxClassShift) return Error::kInvalidArg;
    std::lock_guard<std::mutex> lock(self->mu_);
    if (self->class_chunks_[shift] < pc.initial_count) {
      auto g = self->GrowLocked(shift, pc.initial_count - self->class_chunks_[shift]);
      if (!g) return g.error();
    }
  }

  char name[64];
  std::snprintf(name, sizeof(name), "%s.%d.%08x.sock", RuntimeImpl::HashPrefix(self->hash_).c_str(),
                static_cast<int>(::getpid()), static_cast<uint32_t>(RandomU64()));
  self->socket_path_ = rt->ServiceDir() + "/" + name;
  std::string tmp_path = rt->ServiceDir() + "/." + name + ".tmp";
  struct sockaddr_un addr {};
  if (tmp_path.size() >= sizeof(addr.sun_path)) return Error::kInvalidArg;
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, tmp_path.c_str(), tmp_path.size() + 1);

  self->listen_ = UniqueFd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!self->listen_.Valid()) return Error::kSystem;
  ::unlink(tmp_path.c_str());
  if (::bind(self->listen_.Get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    return errno == EACCES ? Error::kPermission : Error::kSystem;
  }
  ::chmod(tmp_path.c_str(), rt->SocketMode());
  if (::listen(self->listen_.Get(), 128) != 0 || ::rename(tmp_path.c_str(), self->socket_path_.c_str()) != 0) {
    ::unlink(tmp_path.c_str());
    return Error::kSystem;
  }
  bool added = false;
  rt->Call([&] { added = rt->AddFd(self->listen_.Get(), EPOLLIN, self); });
  if (!added) {
    ::unlink(self->socket_path_.c_str());
    return Error::kSystem;
  }
  return self;
}

PublisherImpl::~PublisherImpl() = default;

void PublisherImpl::Shutdown() noexcept {
  std::vector<std::shared_ptr<PubConn>> conns;
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (shutting_down_) return;
    shutting_down_ = true;
    conns = conns_;
    for (auto& c : conns) {
      if (c->hdr != nullptr) c->hdr->closed.store(1, std::memory_order_release);
      c->state = PubConn::State::kClosed;
      WakeSpace(*c);
    }
    // Blocked Publish() calls see shutting_down_ on their next retry.
    idle_cv_.wait(lock, [this] { return blocked_publishers_ == 0; });
  }
  ::unlink(socket_path_.c_str());
  rt_->Call([&] {
    rt_->RemoveFd(listen_.Get());
    for (auto& c : conns) {
      rt_->RemoveFd(c->sock.Get());
      ::shutdown(c->sock.Get(), SHUT_RDWR);
    }
  });
  std::lock_guard<std::mutex> lock(mu_);
  conns_.clear();
}

// ------------------------------------------------------------------ pool

Result<uint32_t> PublisherImpl::GrowLocked(uint32_t class_shift, uint32_t exact_count) {
  if (segments_.size() >= kMaxSegmentsPerPublisher) return Error::kNoMemory;
  uint64_t stride = (1ull << class_shift) + kChunkHeaderSize;
  // On demand the class roughly doubles; eager pools ask for an exact size.
  uint32_t want = exact_count != 0 ? exact_count : std::max(InitialChunks(class_shift), class_chunks_[class_shift]);
  uint64_t budget = options_.max_bytes > segment_bytes_ ? options_.max_bytes - segment_bytes_ : 0;
  uint64_t fit = budget / stride;
  if (exact_count != 0 && fit < exact_count) return Error::kNoMemory;
  uint32_t count = static_cast<uint32_t>(std::min<uint64_t>({want, fit, kMaxChunksPerSegment}));
  if (count == 0) return Error::kNoMemory;
  uint64_t size = stride * count;
  auto seg = CreateSegment("ibmw_shm_data", size, true, options_.prefault);
  if (!seg) return seg.error();

  auto ps = std::make_unique<PoolSegment>();
  ps->id = static_cast<uint32_t>(segments_.size());
  ps->class_shift = class_shift;
  ps->count = count;
  ps->stride = stride;
  ps->seg = std::move(seg.value());
  ps->refcount.assign(count, 0);
  ps->generation.assign(count, 0);
  ps->state.assign(count, static_cast<uint8_t>(ChunkState::kFree));
  read_only_enforced_ = read_only_enforced_ && ps->seg.read_only_enforced;
  for (uint32_t i = count; i-- > 0;) {
    free_[class_shift].push_back((static_cast<uint64_t>(ps->id) << 32) | i);
  }
  class_chunks_[class_shift] += count;
  segment_bytes_ += size;

  const PoolSegment& ref = *ps;
  segments_.push_back(std::move(ps));
  for (auto& c : conns_) {
    c->outstanding.emplace_back((count + 63) / 64, 0);
    c->segment_acked.push_back(0);
    if (c->state == PubConn::State::kAwaitConnAck || c->state == PubConn::State::kActive) {
      if (!SendSegmentLocked(*c, ref)) MarkCorruptLocked(*c);
    }
  }
  return ref.id;
}

bool PublisherImpl::SendSegmentLocked(PubConn& conn, const PoolSegment& seg) noexcept {
  SegmentAddMsg m{};
  m.h = {kMsgMagic, MsgType::kSegmentAdd};
  m.segment_id = seg.id;
  m.chunk_count = seg.count;
  m.chunk_stride = seg.stride;
  m.segment_size = seg.seg.map.Size();
  int fd = seg.seg.fd.Get();
  return SendMessage(conn.sock.Get(), &m, sizeof(m), &fd, 1);
}

bool PublisherImpl::LocateLocked(void* user_payload, uint32_t* seg, uint32_t* idx) noexcept {
  auto* p = static_cast<uint8_t*>(user_payload);
  for (auto& s : segments_) {
    if (!s->seg.map.Contains(p)) continue;
    uint64_t off = static_cast<uint64_t>(p - s->seg.map.Data());
    uint32_t i = static_cast<uint32_t>(off / s->stride);
    auto* h = reinterpret_cast<ChunkHeader*>(s->seg.map.Data() + i * s->stride);
    if (h->magic != kChunkMagic || p != reinterpret_cast<uint8_t*>(h) + h->payload_offset) return false;
    *seg = s->id;
    *idx = i;
    return true;
  }
  return false;
}

void PublisherImpl::DropRefLocked(uint32_t seg, uint32_t idx) noexcept {
  auto& s = *segments_[seg];
  if (s.refcount[idx] == 0) return;  // unreachable: Reclaim guards every decrement
  if (--s.refcount[idx] == 0) {
    s.state[idx] = static_cast<uint8_t>(ChunkState::kFree);
    free_[s.class_shift].push_back((static_cast<uint64_t>(seg) << 32) | idx);
  }
}

// The only way a connection gives up its reference: return, eviction and
// hang-up all come here, so a chunk is dropped at most once per connection.
bool PublisherImpl::ReclaimLocked(PubConn& conn, uint64_t handle) noexcept {
  uint32_t seg = HandleSegment(handle);
  uint32_t idx = HandleIndex(handle);
  if (seg >= segments_.size() || seg >= conn.outstanding.size() || idx >= segments_[seg]->count ||
      segments_[seg]->generation[idx] != HandleGeneration(handle)) {
    ++stats_.bad_returns;
    return false;
  }
  uint64_t& word = conn.outstanding[seg][idx / 64];
  uint64_t bit = 1ull << (idx % 64);
  if ((word & bit) == 0) {
    ++stats_.bad_returns;
    return false;
  }
  word &= ~bit;
  DropRefLocked(seg, idx);
  return true;
}

void PublisherImpl::ReclaimAllLocked(PubConn& conn) noexcept {
  for (uint32_t seg = 0; seg < conn.outstanding.size(); ++seg) {
    for (size_t w = 0; w < conn.outstanding[seg].size(); ++w) {
      uint64_t bits = conn.outstanding[seg][w];
      while (bits != 0) {
        uint32_t b = static_cast<uint32_t>(__builtin_ctzll(bits));
        bits &= bits - 1;
        DropRefLocked(seg, static_cast<uint32_t>(w * 64 + b));
        ++stats_.reclaimed;
      }
      conn.outstanding[seg][w] = 0;
    }
  }
  conn.pending.clear();
}

void PublisherImpl::DrainReturnsLocked(PubConn& conn) noexcept {
  if (conn.hdr == nullptr || conn.state != PubConn::State::kActive) return;
  uint64_t w = conn.hdr->return_write.load(std::memory_order_acquire);
  if (w == conn.return_read) return;
  if (w < conn.return_read || w - conn.return_read > conn.return_slots) {
    MarkCorruptLocked(conn);
    return;
  }
  auto* slots = ReturnSlots(conn.hdr, conn.delivery_slots);
  for (uint64_t i = conn.return_read; i < w; ++i) {
    ReclaimLocked(conn, slots[i % conn.return_slots].load(std::memory_order_relaxed));
  }
  conn.return_read = w;
  conn.hdr->return_read.store(w, std::memory_order_release);
}

void PublisherImpl::DrainAllLocked() noexcept {
  for (auto& c : conns_) DrainReturnsLocked(*c);
}

void PublisherImpl::MarkCorruptLocked(PubConn& conn) noexcept {
  if (conn.state == PubConn::State::kClosed) return;
  Log("connection corrupted, dropping subscriber", url_);
  conn.corrupted = true;
  conn.state = PubConn::State::kClosed;
  ++stats_.corrupted_connections;
  ReclaimAllLocked(conn);
  WakeSpace(conn);
  // The control thread sees the hang-up and unregisters the socket.
  ::shutdown(conn.sock.Get(), SHUT_RDWR);
}

// ---------------------------------------------------------------- loaning

Result<void*> PublisherImpl::Loan(uint32_t payload_size, uint32_t alignment) noexcept {
  if (!IsPowerOfTwo(alignment) || alignment > kMaxPayloadAlignment) return Error::kInvalidArg;
  uint64_t need = static_cast<uint64_t>(payload_size) + (alignment > kChunkHeaderSize ? alignment - kChunkHeaderSize : 0);
  uint32_t shift = ClassShiftFor(std::max<uint64_t>(need, 1));
  if (shift > kMaxClassShift) return Error::kInvalidArg;

  std::lock_guard<std::mutex> lock(mu_);
  if (shutting_down_) return Error::kShuttingDown;
  if (loans_ >= kMaxChunksAllocatedPerPublisher) return Error::kTooManyLoans;
  if (free_[shift].empty()) DrainAllLocked();
  if (free_[shift].empty()) {
    auto g = GrowLocked(shift);
    if (!g) {
      ++stats_.pool_exhausted;
      return g.error();
    }
  }
  uint64_t slot = free_[shift].back();
  free_[shift].pop_back();
  uint32_t seg = static_cast<uint32_t>(slot >> 32);
  uint32_t idx = static_cast<uint32_t>(slot);
  auto& s = *segments_[seg];
  s.refcount[idx] = 1;
  s.state[idx] = static_cast<uint8_t>(ChunkState::kLoaned);
  ++s.generation[idx];
  ++loans_;
  ++stats_.loans;

  uint8_t* base = s.seg.map.Data() + idx * s.stride;
  uint32_t offset = kChunkHeaderSize;
  if (alignment > kChunkHeaderSize) {
    auto addr = reinterpret_cast<uintptr_t>(base) + kChunkHeaderSize;
    offset = static_cast<uint32_t>(((addr + alignment - 1) & ~(uintptr_t{alignment} - 1)) -
                                   reinterpret_cast<uintptr_t>(base));
  }
  auto* h = reinterpret_cast<ChunkHeader*>(base);
  h->magic = kChunkMagic;
  h->abi = kAbiVersion;
  h->reserved0 = 0;
  h->segment_id = seg;
  h->chunk_index = idx;
  h->generation = s.generation[idx];
  h->payload_size = payload_size;
  h->payload_offset = offset;
  h->payload_alignment = alignment;
  h->sequence = 0;
  h->publish_timestamp_ns = 0;
  std::memcpy(base + offset - sizeof(uint32_t), &offset, sizeof(uint32_t));
  return static_cast<void*>(base + offset);
}

bool PublisherImpl::Release(void* user_payload) noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  uint32_t seg, idx;
  if (!LocateLocked(user_payload, &seg, &idx)) return false;
  if (segments_[seg]->state[idx] != static_cast<uint8_t>(ChunkState::kLoaned)) return false;
  --loans_;
  DropRefLocked(seg, idx);
  return true;
}

// -------------------------------------------------------------- delivery

void PublisherImpl::WakeListener(PubConn& conn) noexcept {
  if (conn.hdr == nullptr || conn.notify == nullptr) return;
  // Pairs with the fence in ListenerImpl::Loop (store-load, Dekker): either
  // we see sleeping == 1 or the listener sees our delivery_write.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  uint32_t slot = conn.hdr->notify_slot.load(std::memory_order_relaxed);
  if (slot >= kMaxListenersPerProcess) return;
  NotifySlot& ns = conn.notify->slot[slot];
  if (ns.sleeping.load(std::memory_order_relaxed) != 0 && ns.sleeping.exchange(0, std::memory_order_acq_rel) != 0) {
    ns.epoch.fetch_add(1, std::memory_order_release);
    FutexWake(&ns.epoch, 1);
  }
}

void PublisherImpl::WakeSpace(PubConn& conn) noexcept {
  if (conn.hdr == nullptr) return;
  conn.hdr->space_epoch.fetch_add(1, std::memory_order_release);
  FutexWake(&conn.hdr->space_epoch, INT_MAX);
}

bool PublisherImpl::PushRingLocked(PubConn& conn, uint64_t handle, bool* full) noexcept {
  *full = false;
  // Draining before every push bounds the subscriber's return ring: it never
  // holds more than queue capacity + held handles (see design doc 3.4).
  DrainReturnsLocked(conn);
  if (conn.state != PubConn::State::kActive) return false;
  ConnHeader* h = conn.hdr;
  uint64_t w = conn.delivery_write;
  uint64_t r = h->delivery_read.load(std::memory_order_acquire);
  // read index is subscriber-writable: clamp to [w - capacity, w].
  if (r > w || w - r > conn.capacity) {
    MarkCorruptLocked(conn);
    return false;
  }
  if (w - r == conn.capacity) {
    if (conn.blocking) {
      *full = true;
      return false;
    }
    // Evict the oldest. Whoever wins the CAS on the read index owns it; on
    // failure the subscriber just took it, which also frees a slot.
    if (h->delivery_read.compare_exchange_strong(r, r + 1, std::memory_order_acq_rel)) {
      ReclaimLocked(conn, conn.shadow[r % conn.delivery_slots]);
      ++stats_.evictions;
    }
  }
  // delivery_slots = capacity + 1, so slot w is never the one a consumer
  // holding a stale read index may be loading.
  conn.shadow[w % conn.delivery_slots] = handle;
  DeliverySlots(h)[w % conn.delivery_slots].store(handle, std::memory_order_release);
  conn.delivery_write = w + 1;
  h->delivery_write.store(w + 1, std::memory_order_release);
  return true;
}

void PublisherImpl::FlushPendingLocked(PubConn& conn, bool* delivered) noexcept {
  *delivered = false;
  while (!conn.pending.empty() && conn.state == PubConn::State::kActive) {
    uint64_t hd = conn.pending.front();
    uint32_t seg = HandleSegment(hd);
    if (seg >= conn.segment_acked.size() || conn.segment_acked[seg] == 0) return;
    bool full = false;
    if (!PushRingLocked(conn, hd, &full)) return;
    conn.pending.pop_front();
    *delivered = true;
  }
}

PublisherImpl::Delivery PublisherImpl::DeliverLocked(PubConn& conn, uint64_t handle) noexcept {
  if (conn.state != PubConn::State::kActive && conn.state != PubConn::State::kAwaitConnAck) {
    return Delivery::kSkipped;
  }
  uint32_t seg = HandleSegment(handle);
  uint32_t idx = HandleIndex(handle);
  // Older chunks parked for a segment ack go first; space freed by the
  // subscriber is otherwise only noticed on the next control message.
  bool flushed = false;
  if (conn.state == PubConn::State::kActive && !conn.pending.empty()) FlushPendingLocked(conn, &flushed);
  if (conn.state != PubConn::State::kActive && conn.state != PubConn::State::kAwaitConnAck) {
    return Delivery::kSkipped;  // flushing found the connection corrupt
  }
  bool ready = conn.state == PubConn::State::kActive && conn.pending.empty() && conn.segment_acked[seg] != 0;
  bool full = false;
  if (ready) {
    if (!PushRingLocked(conn, handle, &full)) {
      if (full) return flushed ? Delivery::kFullFlushed : Delivery::kFull;
      return Delivery::kSkipped;
    }
  } else {
    // Subscriber has not mapped this segment yet: hold the chunk privately.
    // For a lossless subscriber the ring and the private list share one
    // capacity, otherwise a growing pool would buffer up to twice the queue
    // without blocking. A segment ack flushes and wakes blocked producers.
    size_t queued = conn.pending.size();
    if (conn.blocking && conn.state == PubConn::State::kActive) {
      uint64_t r = conn.hdr->delivery_read.load(std::memory_order_acquire);
      uint64_t w = conn.delivery_write;
      if (r > w || w - r > conn.capacity) {  // same clamp as PushRingLocked
        MarkCorruptLocked(conn);
        return Delivery::kSkipped;
      }
      queued += static_cast<size_t>(w - r);
    }
    if (queued >= conn.capacity) {
      if (conn.blocking) return flushed ? Delivery::kFullFlushed : Delivery::kFull;
      ReclaimLocked(conn, conn.pending.front());
      conn.pending.pop_front();
      ++stats_.evictions;
    }
    conn.pending.push_back(handle);
  }
  conn.outstanding[seg][idx / 64] |= 1ull << (idx % 64);
  ++segments_[seg]->refcount[idx];
  ++stats_.deliveries;
  return ready || flushed ? Delivery::kDelivered : Delivery::kQueued;
}

bool PublisherImpl::Publish(void* user_payload) noexcept {
  std::vector<std::shared_ptr<PubConn>> wake;
  std::vector<std::shared_ptr<PubConn>> waiting;
  uint32_t seg = 0, idx = 0;
  uint64_t handle = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!LocateLocked(user_payload, &seg, &idx)) return false;
    auto& s = *segments_[seg];
    if (s.state[idx] != static_cast<uint8_t>(ChunkState::kLoaned)) return false;
    s.state[idx] = static_cast<uint8_t>(ChunkState::kPublished);
    --loans_;
    ++stats_.publishes;
    auto* h = reinterpret_cast<ChunkHeader*>(s.seg.map.Data() + idx * s.stride);
    h->sequence = ++sequence_;
    h->publish_timestamp_ns = NowNs();
    handle = MakeHandle(seg, idx, s.generation[idx]);
    // Keeps every return ring below its capacity (see design doc 3.4).
    DrainAllLocked();
    if (!shutting_down_) {
      for (auto& c : conns_) {
        switch (DeliverLocked(*c, handle)) {
          case Delivery::kDelivered: wake.push_back(c); break;
          case Delivery::kFullFlushed:
            wake.push_back(c);
            [[fallthrough]];
          case Delivery::kFull:
            if (stop_blocking_) {
              ++stats_.block_timeouts;
            } else {
              waiting.push_back(c);
            }
            break;
          default: break;
        }
      }
    }
    if (waiting.empty()) {
      DropRefLocked(seg, idx);
    } else {
      ++stats_.blocked;
      ++blocked_publishers_;
    }
  }
  for (auto& c : wake) WakeListener(*c);
  if (waiting.empty()) return true;

  // Lossless subscribers are full: wait for space without holding the lock.
  const bool forever = options_.block_timeout.count() <= 0;
  const uint64_t deadline = NowNs() + static_cast<uint64_t>(options_.block_timeout.count()) * 1000000ull;
  while (!waiting.empty()) {
    for (auto it = waiting.begin(); it != waiting.end();) {
      PubConn& c = **it;
      uint32_t e = c.hdr->space_epoch.load(std::memory_order_acquire);
      c.hdr->space_sleeping.store(1, std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_seq_cst);
      Delivery d;
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_blocking_ && !shutting_down_) ++stats_.block_timeouts;
        d = shutting_down_ || stop_blocking_ ? Delivery::kSkipped : DeliverLocked(c, handle);
      }
      if (d == Delivery::kFullFlushed) WakeListener(c);
      if (d != Delivery::kFull && d != Delivery::kFullFlushed) {
        // space_sleeping is shared by every producer blocked on this
        // subscriber; only the consumer clears it, otherwise we could cancel
        // another producer's announcement and it would never be woken.
        if (d == Delivery::kDelivered) WakeListener(c);
        it = waiting.erase(it);
        continue;
      }
      int64_t timeout = -1;
      if (!forever) {
        uint64_t now = NowNs();
        if (now >= deadline) {
          std::lock_guard<std::mutex> lock(mu_);
          ++stats_.block_timeouts;
          it = waiting.erase(it);
          continue;
        }
        timeout = static_cast<int64_t>(deadline - now);
      }
      FutexWait(&c.hdr->space_epoch, e, timeout);
      ++it;
    }
  }
  std::lock_guard<std::mutex> lock(mu_);
  DropRefLocked(seg, idx);
  // Notify under the lock: once it is released Shutdown() may destroy us.
  if (--blocked_publishers_ == 0) idle_cv_.notify_all();
  return true;
}

void PublisherImpl::StopBlocking() noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  stop_blocking_ = true;
  for (auto& c : conns_) WakeSpace(*c);
}

bool PublisherImpl::HasSubscribers() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto& c : conns_) {
    if (c->state == PubConn::State::kActive) return true;
  }
  return false;
}

PublisherStats PublisherImpl::Stats() const noexcept {
  std::lock_guard<std::mutex> lock(mu_);
  PublisherStats s = stats_;
  s.connections = 0;
  for (auto& c : conns_) s.connections += c->state == PubConn::State::kActive ? 1 : 0;
  s.segments = static_cast<uint32_t>(segments_.size());
  s.segment_bytes = segment_bytes_;
  s.read_only_enforced = read_only_enforced_;
  return s;
}

// ------------------------------------------------------- control thread

void PublisherImpl::OnEvent(uint32_t /*events*/) noexcept {
  for (;;) {
    int fd = ::accept4(listen_.Get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) continue;
      return;
    }
    auto conn = std::make_shared<PubConn>();
    conn->sock = UniqueFd(fd);
    conn->pub = weak_from_this();
    if (!GetPeerCredentials(fd, &conn->peer) || !rt_->PeerAllowed(conn->peer)) {
      Log("rejected peer: credentials not allowed", url_);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (shutting_down_) return;
      if (conns_.size() >= kMaxConnectionsPerPublisher) {
        Log("rejected peer: too many connections", url_);
        continue;
      }
      conns_.push_back(conn);
    }
    if (!rt_->AddFd(fd, EPOLLIN | EPOLLRDHUP, conn)) {
      std::lock_guard<std::mutex> lock(mu_);
      conns_.erase(std::find(conns_.begin(), conns_.end(), conn));
    }
  }
}

void PubConn::OnEvent(uint32_t events) noexcept {
  auto self = shared_from_this();
  auto p = pub.lock();
  if (!p) return;
  if ((events & EPOLLIN) != 0) {
    alignas(8) uint8_t buf[kMaxMsgSize];
    for (;;) {
      std::vector<UniqueFd> fds;
      ssize_t n = ReceiveMessage(sock.Get(), buf, sizeof(buf), &fds);
      if (n < 0 && errno == EAGAIN) break;
      if (n <= 0) {
        p->HandleHangup(self);
        return;
      }
      p->HandleMessage(self, buf, static_cast<size_t>(n), fds);
    }
  }
  if ((events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) != 0) p->HandleHangup(self);
}

void PublisherImpl::HandleMessage(const std::shared_ptr<PubConn>& conn, const uint8_t* data, size_t len,
                                  std::vector<UniqueFd>& fds) noexcept {
  MsgHeader mh{};
  if (len < sizeof(mh)) return HandleHangup(conn);
  std::memcpy(&mh, data, sizeof(mh));
  if (mh.magic != kMsgMagic) return HandleHangup(conn);

  if (mh.type == MsgType::kHello) {
    HelloMsg hello{};
    constexpr size_t kFixed = offsetof(HelloMsg, url);
    if (len < kFixed) return HandleHangup(conn);
    std::memcpy(&hello, data, std::min(len, sizeof(hello)));
    Error reject = Error::kOk;
    if (hello.abi != kAbiVersion) {
      reject = Error::kAbiMismatch;
    } else if (hello.url_length > kMaxUrlLength || len < kFixed + hello.url_length ||
               std::string_view(hello.url, hello.url_length) != url_ || fds.size() != 1) {
      reject = Error::kInvalidArg;
    }
    Mapping notify;
    if (reject == Error::kOk) {
      auto m = MapSegment(fds[0].Get(), kNotifySegmentSize, true);
      if (!m || reinterpret_cast<NotifySegment*>(m->Data())->magic != kNotifyMagic) {
        reject = Error::kCorrupted;
      } else {
        notify = std::move(m.value());
      }
    }
    if (reject != Error::kOk) {
      RejectMsg rm{{kMsgMagic, MsgType::kReject}, static_cast<uint32_t>(reject)};
      SendMessage(conn->sock.Get(), &rm, sizeof(rm), nullptr, 0);
      return HandleHangup(conn);
    }
    uint32_t cap = std::clamp<uint32_t>(hello.queue_capacity, 1, kMaxSubscriberQueueCapacity);
    uint32_t held = std::clamp<uint32_t>(hello.max_held, 1, kMaxChunksHeldPerSubscriber);
    uint32_t dslots = cap + 1;
    uint32_t rslots = cap + held + 1;
    size_t size = ConnSegmentSize(dslots, rslots);
    auto cs = CreateSegment("ibmw_shm_conn", size, false, true);
    if (!cs) return HandleHangup(conn);

    std::lock_guard<std::mutex> lock(mu_);
    if (conn->state != PubConn::State::kAwaitHello || shutting_down_) return;
    conn->conn_seg = std::move(cs.value());
    conn->hdr = new (conn->conn_seg.map.Data()) ConnHeader{};
    ConnHeader* h = conn->hdr;
    h->magic = kConnMagic;
    h->abi = kAbiVersion;
    h->queue_capacity = cap;
    h->delivery_slots = dslots;
    h->return_slots = rslots;
    h->notify_slot.store(kNoListener, std::memory_order_relaxed);
    conn->notify_map = std::move(notify);
    conn->notify = reinterpret_cast<NotifySegment*>(conn->notify_map.Data());
    conn->capacity = cap;
    conn->delivery_slots = dslots;
    conn->return_slots = rslots;
    conn->sub_blocking = hello.queue_full == static_cast<uint32_t>(QueueFullPolicy::kBlockProducer);
    conn->blocking = conn->sub_blocking && options_.too_slow == TooSlowPolicy::kWaitForConsumer;
    conn->shadow.assign(dslots, 0);
    conn->outstanding.clear();
    conn->segment_acked.clear();
    for (auto& s : segments_) {
      conn->outstanding.emplace_back((s->count + 63) / 64, 0);
      conn->segment_acked.push_back(0);
    }
    WelcomeMsg wm{};
    wm.h = {kMsgMagic, MsgType::kWelcome};
    wm.abi = kAbiVersion;
    wm.queue_capacity = cap;
    wm.delivery_slots = dslots;
    wm.return_slots = rslots;
    wm.segment_size = size;
    int fd = conn->conn_seg.fd.Get();
    bool ok = SendMessage(conn->sock.Get(), &wm, sizeof(wm), &fd, 1);
    for (auto& s : segments_) ok = ok && SendSegmentLocked(*conn, *s);
    // The subscriber holds its own descriptor now; keep only the mapping.
    conn->conn_seg.fd.Reset();
    if (!ok) {
      MarkCorruptLocked(*conn);
      return;
    }
    conn->state = PubConn::State::kAwaitConnAck;
    return;
  }

  bool delivered = false;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (mh.type == MsgType::kConnAck && conn->state == PubConn::State::kAwaitConnAck) {
      conn->state = PubConn::State::kActive;
    } else if (mh.type == MsgType::kSegmentAck && len >= sizeof(SegmentAckMsg) &&
               conn->state != PubConn::State::kClosed) {
      SegmentAckMsg am{};
      std::memcpy(&am, data, sizeof(am));
      if (am.segment_id >= conn->segment_acked.size()) {
        MarkCorruptLocked(*conn);
        return;
      }
      conn->segment_acked[am.segment_id] = 1;
    } else {
      MarkCorruptLocked(*conn);
      return;
    }
    FlushPendingLocked(*conn, &delivered);
    if (conn->blocking) WakeSpace(*conn);
  }
  if (delivered) WakeListener(*conn);
}

void PublisherImpl::HandleHangup(const std::shared_ptr<PubConn>& conn) noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (conn->state != PubConn::State::kClosed) {
      conn->state = PubConn::State::kClosed;
      ReclaimAllLocked(*conn);
      WakeSpace(*conn);
    }
    auto it = std::find(conns_.begin(), conns_.end(), conn);
    if (it != conns_.end()) conns_.erase(it);
  }
  rt_->RemoveFd(conn->sock.Get());
}

}  // namespace detail
}  // namespace ibmw::selfdev::shm
