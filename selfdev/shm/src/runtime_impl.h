// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <sys/types.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ibmw_selfdev/shm/runtime.h"
#include "platform.h"
#include "protocol.h"

namespace ibmw::selfdev::shm::detail {

class SubscriberImpl;

// Something registered with the control thread's epoll.
class EventHandler {
 public:
  virtual ~EventHandler() = default;
  // Runs on the control thread. `events` are EPOLL* bits.
  virtual void OnEvent(uint32_t events) noexcept = 0;
};

// Process-wide state. Created lazily; never destroyed except by Shutdown().
//
// Threading: the epoll handler table is only touched on the control thread,
// other threads reach it through Call()/Post(). The subscriber registry and
// listener slot table have their own mutex.
class RuntimeImpl {
 public:
  // Returns the runtime, initialising it with defaults if needed; nullptr
  // when initialisation failed.
  static RuntimeImpl* Instance() noexcept;
  static Result<void> Init(const RuntimeOptions& options);
  static bool IsInitialized() noexcept;
  static void Shutdown() noexcept;

  ~RuntimeImpl();

  const std::string& ServiceDir() const noexcept { return service_dir_; }
  bool OnControlThread() const noexcept;
  // Runs fn on the control thread and waits for it. Inline if already there.
  void Call(const std::function<void()>& fn);
  void Post(std::function<void()> fn);

  // Control thread only.
  bool AddFd(int fd, uint32_t events, std::shared_ptr<EventHandler> handler);
  void RemoveFd(int fd);

  NotifySegment* Notify() const noexcept { return reinterpret_cast<NotifySegment*>(notify_.map.Data()); }
  int NotifyFd() const noexcept { return notify_.fd.Get(); }
  std::optional<uint32_t> AllocListenerSlot();
  void FreeListenerSlot(uint32_t slot);

  void RegisterSubscriber(uint64_t hash, const std::shared_ptr<SubscriberImpl>& sub);
  void UnregisterSubscriber(uint64_t hash, const SubscriberImpl* sub);
  // Asks every subscriber of `hash` to connect to the socket at `path`.
  // Control thread only.
  void OfferPublisher(uint64_t hash, const std::string& path);
  // Scans the service directory for publishers of `hash`. Control thread only.
  void ScanFor(uint64_t hash);

  bool PeerAllowed(const PeerCredentials& cred) const noexcept;
  mode_t SocketMode() const noexcept { return group_ ? 0660 : 0600; }

  static std::string HashPrefix(uint64_t hash);

 private:
  RuntimeImpl() = default;
  Result<void> Start(const RuntimeOptions& options);
  // root + owner + domain after environment overrides; "" if invalid.
  static std::string ResolveServiceDir(const RuntimeOptions& options);
  void Loop();
  void HandleInotify();
  void RunCommands();

  RuntimeOptions options_;
  std::string service_dir_;
  std::optional<gid_t> group_;
  uid_t euid_ = 0;

  UniqueFd epoll_;
  UniqueFd wake_;  // eventfd
  UniqueFd inotify_;
  Segment notify_;
  std::thread thread_;
  std::thread::id thread_id_;
  bool running_ = true;  // control thread only

  struct Registered {
    uint32_t generation;
    std::shared_ptr<EventHandler> handler;
  };
  std::unordered_map<int, Registered> handlers_;  // control thread only
  uint32_t next_generation_ = 1;

  std::mutex cmd_mu_;
  std::deque<std::function<void()>> commands_;

  mutable std::mutex reg_mu_;
  std::multimap<uint64_t, std::weak_ptr<SubscriberImpl>> subscribers_;
  uint64_t listener_slots_used_ = 0;
};

}  // namespace ibmw::selfdev::shm::detail
