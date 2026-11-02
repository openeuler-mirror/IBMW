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
  void Call(const std::function<void()>& fn);
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
  // an idle process still never wakes up.
  bool RunAfter(uint32_t delay_ms, std::function<void()> fn) noexcept;

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
  // ScanFor() every registered subscriber hash. Control thread only.
  void ScanAll();

  bool PeerAllowed(const PeerCredentials& cred) const noexcept;
  mode_t SocketMode() const noexcept { return group_ ? 0660 : 0600; }

  static std::string HashPrefix(uint64_t hash);

 private:
  RuntimeImpl() = default;
  Result<void> Start(const RuntimeOptions& options);
  // root + owner + domain; each of root, domain and group is the explicit
  // option if set, else its environment variable, else the default. Logs and
  // returns kInvalidArg when the result is unusable.
  static Result<std::string> ResolveServiceDir(const RuntimeOptions& options, std::optional<gid_t>* group,
                                               std::string* root = nullptr, std::string* domain = nullptr);
  void Loop();
  void HandleInotify();
  // Re-creates the service directory and its watch after it was removed or
  // moved away, then rescans. Retries with backoff while that fails.
  void Rewatch(uint32_t attempt);
  void RunCommands();
  void RunTimers();

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
