// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <sys/types.h>

#include <atomic>
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

// Longest service directory: publisher socket files are created as
// "<dir>/.<16 hex>.<pid>.<8 hex>.sock.tmp" and must fit sockaddr_un.sun_path
// (108 bytes incl. NUL) with a pid of up to 7 digits (Linux PID_MAX_LIMIT).
inline constexpr size_t kMaxServiceDirLength = 107 - (2 + 16 + 1 + 7 + 1 + 8 + 5 + 4);

// Names of "ibmw_shm.<owner>.<domain>" directories under `root` whose owner
// part differs from `owner`: processes there use the same domain under
// another uid/group and can never meet this one.
std::vector<std::string> ForeignServiceDirs(const std::string& root, const std::string& owner,
                                            const std::string& domain);

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
  // when initialisation failed (LastInitError() tells why; never kOk).
  static RuntimeImpl* Instance() noexcept;
  static Error LastInitError() noexcept;
  static Result<void> Init(const RuntimeOptions& options);
  static bool IsInitialized() noexcept;
  static void Shutdown() noexcept;

  ~RuntimeImpl();

  const std::string& ServiceDir() const noexcept { return service_dir_; }
  bool OnControlThread() const noexcept;
  // Runs fn on the control thread and waits for it. Inline if already there.
  // What fn throws is rethrown here; failing to queue it (std::bad_alloc)
  // is retried, so a non-throwing fn makes Call() non-throwing.
  void Call(const std::function<void()>& fn);
  // Queues fn for the control thread; throws std::bad_alloc. Exceptions fn
  // throws there are logged and dropped.
  void Post(std::function<void()> fn);

  // Control thread only. `tag` receives the epoll tag for ModFd().
  bool AddFd(int fd, uint32_t events, std::shared_ptr<EventHandler> handler, uint64_t* tag = nullptr);
  void RemoveFd(int fd);
  // Changes the events of a registered fd. Any thread, but the caller must
  // guarantee `fd` is still registered under `tag` and open (e.g. it holds
  // the connection that owns the fd and RemoveFd() has not run yet).
  void ModFd(int fd, uint64_t tag, uint32_t events) noexcept;
  // Runs fn on the control thread after `delay_ms`. Control thread only.
  // Pending timers are the only thing that gives epoll_wait a timeout, so
  // an idle process still never wakes up. False when out of memory.
  bool RunAfter(uint32_t delay_ms, std::function<void()> fn) noexcept;
  // Rescans the service directory for every subscriber later (backoff 10 ms
  // doubling to 1 s while scans keep failing). Discovery that failed (out
  // of memory, opendir error) calls this so no publisher is lost for good.
  // Needs no allocation. Control thread only.
  void ScheduleRescan() noexcept;

  NotifySegment* Notify() const noexcept { return reinterpret_cast<NotifySegment*>(notify_.map.Data()); }
  int NotifyFd() const noexcept { return notify_.fd.Get(); }
  std::optional<uint32_t> AllocListenerSlot();
  void FreeListenerSlot(uint32_t slot);

  void RegisterSubscriber(uint64_t hash, const std::shared_ptr<SubscriberImpl>& sub);
  void UnregisterSubscriber(uint64_t hash, const SubscriberImpl* sub);
  // Asks every subscriber of `hash` to connect to the socket at `path`.
  // Control thread only. These three schedule a rescan when they fail.
  void OfferPublisher(uint64_t hash, const std::string& path) noexcept;
  // Scans the service directory for publishers of `hash`. Control thread only.
  void ScanFor(uint64_t hash) noexcept;
  // ScanFor() every registered subscriber hash. Control thread only.
  void ScanAll() noexcept;

  // Same euid, root, or (group mode) the group as effective gid or, where
  // the kernel reports it (SO_PEERGROUPS), as a supplementary group.
  bool PeerAllowed(const PeerCredentials& cred, int sock) const noexcept;
  // Applies the socket file's group and mode (0600, or 0660 and the
  // group); false with errno set when that fails.
  bool SetSocketAccess(const char* path) const noexcept;

  static std::string HashPrefix(uint64_t hash);

 private:
  RuntimeImpl() = default;
  Result<void> Start(const RuntimeOptions& options);
  // root + owner + domain; each of root, domain and group is the explicit
  // option if set, else its environment variable, else the default. Logs and
  // returns kInvalidArg when the result is unusable.
  static Result<std::string> ResolveServiceDir(const RuntimeOptions& options, std::optional<gid_t>* group,
                                               std::string* root = nullptr, std::string* domain = nullptr);
  void Loop() noexcept;
  void HandleInotify() noexcept;
  // Re-creates the service directory and its watch after it was removed or
  // moved away, then rescans. Retries with backoff while that fails.
  void Rewatch() noexcept;
  void RunCommands() noexcept;
  void RunTimers() noexcept;
  // Runs a due rescan or re-watch.
  void RunDeferred() noexcept;

  RuntimeOptions options_;
  std::string service_dir_;
  std::optional<gid_t> group_;
  uid_t euid_ = 0;

  UniqueFd epoll_;
  UniqueFd wake_;  // eventfd
  UniqueFd inotify_;
  int watch_ = -1;  // inotify watch descriptor of service_dir_, control thread only
  Segment notify_;
  std::thread thread_;
  std::thread::id thread_id_;
  bool running_ = true;  // control thread only
  // Set by the destructor; the control thread stops once it has run the
  // commands queued before. A flag, not a command: stopping must not need
  // an allocation (Runtime::Shutdown() is noexcept).
  std::atomic<bool> stop_{false};
  // Retries that must not depend on an allocation succeeding (control
  // thread only): deadline (NowNs, 0: none) and attempt for the backoff.
  uint64_t rescan_at_ = 0;
  uint32_t rescan_attempt_ = 0;
  uint64_t rewatch_at_ = 0;
  uint32_t rewatch_attempt_ = 0;

  struct Registered {
    uint32_t generation;
    std::shared_ptr<EventHandler> handler;
  };
  std::unordered_map<int, Registered> handlers_;  // control thread only
  uint32_t next_generation_ = 1;
  std::multimap<uint64_t, std::function<void()>> timers_;  // deadline (NowNs), control thread only

  std::mutex cmd_mu_;
  std::deque<std::function<void()>> commands_;

  mutable std::mutex reg_mu_;
  std::multimap<uint64_t, std::weak_ptr<SubscriberImpl>> subscribers_;
  uint64_t listener_slots_used_ = 0;
};

}  // namespace ibmw::selfdev::shm::detail
