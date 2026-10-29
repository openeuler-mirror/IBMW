// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ibmw_selfdev/shm/chunk.h"
#include "ibmw_selfdev/shm/listener.h"
#include "ibmw_selfdev/shm/publisher.h"
#include "ibmw_selfdev/shm/subscriber.h"
#include "platform.h"
#include "protocol.h"
#include "runtime_impl.h"

namespace ibmw::selfdev::shm::detail {

// ============================================================ publisher side

// One pool segment: `count` chunks of `stride` bytes (header + payload class).
// All bookkeeping lives in publisher-private memory; the shared mapping only
// holds chunk headers and payloads.
struct PoolSegment {
  uint32_t id = 0;
  uint32_t class_shift = 0;
  uint32_t count = 0;
  uint64_t stride = 0;
  Segment seg;
  std::vector<uint32_t> refcount;
  std::vector<uint32_t> generation;
  std::vector<uint8_t> state;  // ChunkState
};

enum class ChunkState : uint8_t { kFree, kLoaned, kPublished };

class PublisherImpl;

// Publisher end of one connection. Fields other than `sock` are guarded by
// the owning publisher's mutex.
class PubConn : public EventHandler, public std::enable_shared_from_this<PubConn> {
 public:
  enum class State : uint8_t { kAwaitHello, kAwaitConnAck, kActive, kClosed };

  void OnEvent(uint32_t events) noexcept override;

  std::weak_ptr<PublisherImpl> pub;
  UniqueFd sock;
  PeerCredentials peer;
  State state = State::kAwaitHello;
  bool corrupted = false;

  Segment conn_seg;
  ConnHeader* hdr = nullptr;
  Mapping notify_map;
  NotifySegment* notify = nullptr;
  uint32_t capacity = 1;
  uint32_t delivery_slots = 2;
  uint32_t return_slots = 2;
  bool blocking = false;
  bool sub_blocking = false;
  std::vector<uint64_t> shadow;  // what we wrote into each delivery slot
  uint64_t delivery_write = 0;   // private copy; the shared one is output only
  uint64_t return_read = 0;
  std::vector<std::vector<uint64_t>> outstanding;  // [segment][chunk bit]
  std::vector<uint8_t> segment_acked;
  std::deque<uint64_t> pending;  // delivered but waiting for segment ack
};

class PublisherImpl : public EventHandler, public std::enable_shared_from_this<PublisherImpl> {
 public:
  static Result<std::shared_ptr<PublisherImpl>> Create(std::string_view url, const PublisherOptions& options);
  ~PublisherImpl() override;

  Result<void*> Loan(uint32_t payload_size, uint32_t alignment) noexcept;
  bool Publish(void* user_payload) noexcept;
  bool Release(void* user_payload) noexcept;
  void StopBlocking() noexcept;
  bool HasSubscribers() const noexcept;
  PublisherStats Stats() const noexcept;
  void Shutdown() noexcept;

  // Listening socket (control thread).
  void OnEvent(uint32_t events) noexcept override;
  // Connection callbacks (control thread).
  void HandleMessage(const std::shared_ptr<PubConn>& conn, const uint8_t* data, size_t len,
                     std::vector<UniqueFd>& fds) noexcept;
  void HandleHangup(const std::shared_ptr<PubConn>& conn) noexcept;

 private:
  enum class Delivery : uint8_t { kDelivered, kQueued, kFull, kFullFlushed, kSkipped };

  PublisherImpl() = default;

  Result<uint32_t> GrowLocked(uint32_t class_shift, uint32_t exact_count = 0);
  bool LocateLocked(void* user_payload, uint32_t* seg, uint32_t* idx) noexcept;
  void DropRefLocked(uint32_t seg, uint32_t idx) noexcept;
  bool ReclaimLocked(PubConn& conn, uint64_t handle) noexcept;
  void DrainReturnsLocked(PubConn& conn) noexcept;
  void DrainAllLocked() noexcept;
  Delivery DeliverLocked(PubConn& conn, uint64_t handle) noexcept;
  bool PushRingLocked(PubConn& conn, uint64_t handle, bool* full) noexcept;
  void FlushPendingLocked(PubConn& conn, bool* delivered) noexcept;
  void MarkCorruptLocked(PubConn& conn) noexcept;
  void ReclaimAllLocked(PubConn& conn) noexcept;
  bool SendSegmentLocked(PubConn& conn, const PoolSegment& seg) noexcept;
  static void WakeListener(PubConn& conn) noexcept;
  static void WakeSpace(PubConn& conn) noexcept;

  RuntimeImpl* rt_ = nullptr;
  std::string url_;
  uint64_t hash_ = 0;
  PublisherOptions options_;
  UniqueFd listen_;
  std::string socket_path_;

  mutable std::mutex mu_;
  bool shutting_down_ = false;
  bool stop_blocking_ = false;  // see Publisher::StopBlocking()
  // Publish() calls waiting for lossless space; Shutdown() waits for them
  // to leave before the object can be destroyed.
  uint32_t blocked_publishers_ = 0;
  std::condition_variable idle_cv_;
  std::vector<std::unique_ptr<PoolSegment>> segments_;
  std::vector<std::vector<uint64_t>> free_;  // per class: (seg << 32) | idx
  std::vector<uint32_t> class_chunks_;       // total chunks per class
  uint64_t segment_bytes_ = 0;
  uint32_t loans_ = 0;
  uint64_t sequence_ = 0;
  std::vector<std::shared_ptr<PubConn>> conns_;
  bool read_only_enforced_ = true;
  PublisherStats stats_;
};

// =========================================================== subscriber side

struct SubSegment {
  Mapping map;
  uint64_t stride = 0;
  uint32_t count = 0;
};

class SubscriberImpl;

class SubConn : public EventHandler, public std::enable_shared_from_this<SubConn> {
 public:
  enum class State : uint8_t { kAwaitWelcome, kActive, kOrphan };

  void OnEvent(uint32_t events) noexcept override;

  std::weak_ptr<SubscriberImpl> sub;
  UniqueFd sock;
  std::string path;
  // Guarded by the subscriber mutex.
  State state = State::kAwaitWelcome;
  Mapping conn_map;
  ConnHeader* hdr = nullptr;
  uint32_t capacity = 1;
  uint32_t delivery_slots = 2;
  uint32_t return_slots = 2;
  uint64_t return_write = 0;
  std::unordered_map<uint32_t, SubSegment> segments;
  uint32_t held = 0;
  // Orphaned by an orderly publisher shutdown: samples already queued stay
  // takeable until the ring is empty.
  bool draining = false;
  std::deque<uint64_t> return_backlog;  // returns that did not fit the ring
};

class SubscriberImpl : public std::enable_shared_from_this<SubscriberImpl> {
 public:
  static Result<std::shared_ptr<SubscriberImpl>> Create(std::string_view url, const SubscriberOptions& options);
  ~SubscriberImpl();

  bool HasData() const noexcept;
  // HasData() plus a counter that grows with every delivery, read atomically.
  bool Poll(uint64_t* version) const noexcept;
  Result<const void*> Take() noexcept;
  void Release(const void* user_payload) noexcept;
  size_t PayloadSize(const void* user_payload) const noexcept;
  SubscriberStats Stats() const noexcept;
  void SetNotifySlot(uint32_t slot) noexcept;
  void Shutdown() noexcept;

  // Control thread.
  void ConnectTo(const std::string& path) noexcept;
  void HandleMessage(const std::shared_ptr<SubConn>& conn, const uint8_t* data, size_t len,
                     std::vector<UniqueFd>& fds) noexcept;
  void HandleHangup(const std::shared_ptr<SubConn>& conn) noexcept;

  uint64_t Hash() const noexcept { return hash_; }

  // Link to the attached listener; a subscriber has at most one.
  std::shared_ptr<ListenerImpl> AttachedListener() const noexcept;
  // Sets the link unless another live listener holds it.
  bool ClaimListener(const std::shared_ptr<ListenerImpl>& listener) noexcept;
  // Clears the link if it still points at `listener`.
  void ClearListener(const ListenerImpl* listener) noexcept;

  Subscriber* owner = nullptr;

 private:
  SubscriberImpl() = default;
  void PushReturnLocked(SubConn& conn, uint64_t handle) noexcept;
  void SignalSpace(SubConn& conn) noexcept;
  void RemoveConnLocked(const SubConn* conn) noexcept;
  // Drops orphaned connections nothing refers to any more.
  void PruneOrphansLocked() noexcept;
  void MarkCorruptLocked(SubConn& conn) noexcept;

  RuntimeImpl* rt_ = nullptr;
  std::string url_;
  uint64_t hash_ = 0;
  SubscriberOptions options_;

  mutable std::mutex mu_;
  bool shutting_down_ = false;
  std::vector<std::shared_ptr<SubConn>> conns_;
  size_t round_robin_ = 0;
  struct Held {
    std::shared_ptr<SubConn> conn;
    uint64_t handle;
    uint32_t size;  // validated in Take()
  };
  std::unordered_map<const void*, Held> held_;
  std::set<std::string> known_paths_;
  std::atomic<uint32_t> notify_slot_{kNoListener};
  uint64_t removed_version_ = 0;
  SubscriberStats stats_;

  mutable std::mutex listener_mu_;
  std::weak_ptr<ListenerImpl> listener_;
};

// ============================================================ listener

class ListenerImpl : public std::enable_shared_from_this<ListenerImpl> {
 public:
  static Result<std::shared_ptr<ListenerImpl>> Create(const ListenerOptions& options);
  ~ListenerImpl();

  Result<void> Attach(Subscriber& sub, Listener::Callback cb, void* ctx) noexcept;
  void Detach(SubscriberImpl* sub) noexcept;
  uint32_t Size() const noexcept;
  void Stop() noexcept;

 private:
  ListenerImpl() = default;
  void Loop() noexcept;
  void Cleanup() noexcept;
  void Wake() noexcept;

  struct Entry {
    Subscriber* subscriber;
    std::shared_ptr<SubscriberImpl> impl;
    Listener::Callback callback;
    void* context;
    uint64_t last_version;
  };

  RuntimeImpl* rt_ = nullptr;
  ListenerOptions options_;
  uint32_t slot_ = kNoListener;
  NotifySlot* notify_ = nullptr;
  std::thread thread_;
  std::thread::id thread_id_;
  std::atomic<bool> stop_{false};

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::vector<Entry> entries_;
  const SubscriberImpl* dispatching_ = nullptr;
};

}  // namespace ibmw::selfdev::shm::detail
