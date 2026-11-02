// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <new>

#include "impl.h"
#include "runtime_impl.h"

namespace ibmw::selfdev::shm {

const char* ErrorName(Error e) noexcept {
  switch (e) {
    case Error::kOk: return "ok";
    case Error::kNoMemory: return "no_memory";
    case Error::kTooManyLoans: return "too_many_loans";
    case Error::kTooManyHeld: return "too_many_held";
    case Error::kNoData: return "no_data";
    case Error::kInvalidArg: return "invalid_arg";
    case Error::kNotConnected: return "not_connected";
    case Error::kShuttingDown: return "shutting_down";
    case Error::kAbiMismatch: return "abi_mismatch";
    case Error::kPermission: return "permission";
    case Error::kTimeout: return "timeout";
    case Error::kCorrupted: return "corrupted";
    case Error::kSystem: return "system";
    case Error::kAlreadyInitialized: return "already_initialized";
  }
  return "unknown";
}

Result<void> Runtime::Init(const RuntimeOptions& options) { return detail::RuntimeImpl::Init(options); }
bool Runtime::IsInitialized() noexcept { return detail::RuntimeImpl::IsInitialized(); }
std::string Runtime::ServiceDirectory() {
  auto* rt = detail::RuntimeImpl::Instance();
  return rt != nullptr ? rt->ServiceDir() : std::string();
}
void Runtime::Shutdown() noexcept { detail::RuntimeImpl::Shutdown(); }

namespace detail {

namespace {

std::mutex g_instance_mu;
RuntimeImpl* g_instance = nullptr;  // intentionally leaked unless Shutdown()
Error g_last_init_error = Error::kOk;  // guarded by g_instance_mu

constexpr uint32_t kEpollMaxEvents = 64;
// epoll user data: generation in the high half, fd in the low half, so an
// event for a closed fd whose number was reused is ignored.
constexpr uint64_t kWakeTag = 0xffffffff00000000ull;
constexpr uint64_t kInotifyTag = 0xfffffffe00000000ull;
constexpr uint32_t kWatchMask = IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;

void LogError(const std::string& what) { std::fprintf(stderr, "[ibmw_shm] %s\n", what.c_str()); }

// Root used when neither RuntimeOptions::root_dir nor $IBMW_SHM_ROOT is set;
// "" when there is none. A fixed path, never derived from per-process
// environment (XDG_RUNTIME_DIR, TMPDIR), so every process of a user meets.
std::string DefaultRoot() {
#if defined(__OHOS__)
  // OHOS (HongMeng or Linux kernel): /tmp is not writable on devices and
  // what an app sandbox can see is untested, so the root must be explicit.
  return {};
#else
  // /dev/shm is tmpfs and not aged by systemd-tmpfiles like /tmp.
  struct stat st {};
  if (::stat("/dev/shm", &st) == 0 && S_ISDIR(st.st_mode) && ::access("/dev/shm", W_OK | X_OK) == 0) {
    return "/dev/shm";
  }
  return "/tmp";
#endif
}

}  // namespace

std::vector<std::string> ForeignServiceDirs(const std::string& root, const std::string& owner,
                                            const std::string& domain) {
  std::vector<std::string> out;
  DIR* d = ::opendir(root.c_str());
  if (d == nullptr) return out;
  const std::string prefix = "ibmw_shm.";
  const std::string suffix = "." + domain;
  while (struct dirent* de = ::readdir(d)) {
    std::string name(de->d_name);
    if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
      continue;
    }
    std::string who = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (who != owner && who.find('.') == std::string::npos) out.push_back(root + "/" + name);
  }
  ::closedir(d);
  return out;
}

RuntimeImpl* RuntimeImpl::Instance() noexcept {
  {
    std::lock_guard<std::mutex> lock(g_instance_mu);
    if (g_instance != nullptr) return g_instance;
  }
  (void)Init(RuntimeOptions{});
  std::lock_guard<std::mutex> lock(g_instance_mu);
  return g_instance;
}

Error RuntimeImpl::LastInitError() noexcept {
  std::lock_guard<std::mutex> lock(g_instance_mu);
  // Never kOk: callers return it as the error of a failed creation.
  return g_last_init_error != Error::kOk ? g_last_init_error : Error::kSystem;
}

Result<void> RuntimeImpl::Init(const RuntimeOptions& options) {
  std::lock_guard<std::mutex> lock(g_instance_mu);
  if (g_instance != nullptr) {
    std::optional<gid_t> group;
    auto dir = ResolveServiceDir(options, &group);
    if (!dir) return dir.error();
    if (*dir == g_instance->ServiceDir()) return {};
    return Error::kAlreadyInitialized;
  }
  RuntimeImpl* rt = nullptr;
  try {
    rt = new RuntimeImpl();
    auto r = rt->Start(options);
    if (!r) {
      delete rt;
      g_last_init_error = r.error();
      return r;
    }
  } catch (...) {  // std::bad_alloc, std::system_error from std::thread
    delete rt;
    g_last_init_error = Error::kNoMemory;
    return Error::kNoMemory;
  }
  g_instance = rt;
  return {};
}

bool RuntimeImpl::IsInitialized() noexcept {
  std::lock_guard<std::mutex> lock(g_instance_mu);
  return g_instance != nullptr;
}

void RuntimeImpl::Shutdown() noexcept {
  RuntimeImpl* rt;
  {
    std::lock_guard<std::mutex> lock(g_instance_mu);
    rt = g_instance;
    g_instance = nullptr;
  }
  delete rt;
}

std::string RuntimeImpl::HashPrefix(uint64_t hash) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016" PRIx64, hash);
  return buf;
}

Result<std::string> RuntimeImpl::ResolveServiceDir(const RuntimeOptions& options, std::optional<gid_t>* group,
                                                    std::string* root_out, std::string* domain_out) {
  auto env = [](const char* name) -> std::string {
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
  };
  std::string root = !options.root_dir.empty() ? options.root_dir : env("IBMW_SHM_ROOT");
  if (root.empty()) root = DefaultRoot();
  if (root.empty()) {
    LogError("no shm root directory: set RuntimeOptions::root_dir or IBMW_SHM_ROOT (required on OHOS)");
    return Error::kInvalidArg;
  }
  std::string domain = !options.domain.empty() ? options.domain : env("IBMW_SHM_DOMAIN");
  if (domain.empty()) domain = "default";
  if (domain.find('/') != std::string::npos || domain.size() > 32) {
    LogError("invalid shm domain '" + domain + "': at most 32 characters, no '/'");
    return Error::kInvalidArg;
  }
  *group = options.group;
  if (!group->has_value()) {
    std::string g = env("IBMW_SHM_GROUP");
    if (!g.empty()) {
      char* end = nullptr;
      unsigned long v = std::strtoul(g.c_str(), &end, 10);
      if (end == g.c_str() || *end != '\0' || v > 0xfffffffful) {
        LogError("invalid IBMW_SHM_GROUP '" + g + "': expected a numeric gid");
        return Error::kInvalidArg;
      }
      *group = static_cast<gid_t>(v);
    }
  }
  std::string owner = group->has_value() ? "g" + std::to_string(**group) : std::to_string(::geteuid());
  std::string dir = root + "/ibmw_shm." + owner + "." + domain;
  if (dir.size() > kMaxServiceDirLength) {
    LogError("service directory '" + dir + "' is " + std::to_string(dir.size()) + " bytes, the limit is " +
             std::to_string(kMaxServiceDirLength) + " (socket paths must fit sun_path): use a root of at most " +
             std::to_string(kMaxServiceDirLength - (dir.size() - root.size())) + " bytes");
    return Error::kInvalidArg;
  }
  if (root_out != nullptr) *root_out = root;
  if (domain_out != nullptr) *domain_out = domain;
  return dir;
}

Result<void> RuntimeImpl::Start(const RuntimeOptions& options) {
  options_ = options;
  euid_ = ::geteuid();
  std::string root, domain;
  auto dir = ResolveServiceDir(options, &group_, &root, &domain);
  if (!dir) return dir.error();
  service_dir_ = std::move(dir.value());
  if (!MakeDirectory(service_dir_, group_ ? 0770 : 0700, group_)) {
    LogError("cannot use service directory '" + service_dir_ + "': " + std::strerror(errno) +
             " (it must be a directory owned by this user, or by the group given as RuntimeOptions::group)");
    return Error::kPermission;
  }
  const std::string owner = group_ ? "g" + std::to_string(*group_) : std::to_string(euid_);
  for (const auto& other : ForeignServiceDirs(root, owner, domain)) {
    LogError("note: '" + other + "' serves domain '" + domain +
             "' for another user or group; processes only meet when they run as the same user or all set the "
             "same RuntimeOptions::group (or IBMW_SHM_GROUP)");
  }

  auto notify = CreateSegment("ibmw_shm_notify", kNotifySegmentSize, false, true);
  if (!notify) return notify.error();
  notify_ = std::move(notify.value());
  auto* ns = Notify();
  ns->magic = kNotifyMagic;
  ns->slots = kMaxListenersPerProcess;
  for (auto& s : ns->slot) {
    s.epoch.store(0, std::memory_order_relaxed);
    s.sleeping.store(0, std::memory_order_relaxed);
  }

  epoll_ = UniqueFd(::epoll_create1(EPOLL_CLOEXEC));
  wake_ = UniqueFd(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
  inotify_ = UniqueFd(::inotify_init1(IN_CLOEXEC | IN_NONBLOCK));
  if (!epoll_.Valid() || !wake_.Valid() || !inotify_.Valid()) return Error::kSystem;
  watch_ = ::inotify_add_watch(inotify_.Get(), service_dir_.c_str(), kWatchMask);
  if (watch_ < 0) return Error::kSystem;

  struct epoll_event ev {};
  ev.events = EPOLLIN;
  ev.data.u64 = kWakeTag;
  if (::epoll_ctl(epoll_.Get(), EPOLL_CTL_ADD, wake_.Get(), &ev) != 0) return Error::kSystem;
  ev.data.u64 = kInotifyTag;
  if (::epoll_ctl(epoll_.Get(), EPOLL_CTL_ADD, inotify_.Get(), &ev) != 0) return Error::kSystem;

  std::promise<std::thread::id> started;
  auto started_future = started.get_future();
  thread_ = std::thread([this, &started] {
    ::pthread_setname_np(::pthread_self(), "ibmw_shm_ctl");
    started.set_value(std::this_thread::get_id());
    Loop();
  });
  thread_id_ = started_future.get();
  return {};
}

RuntimeImpl::~RuntimeImpl() {
  if (thread_.joinable()) {
    Post([this] { running_ = false; });
    thread_.join();
  }
  for (auto& [fd, reg] : handlers_) ::epoll_ctl(epoll_.Get(), EPOLL_CTL_DEL, fd, nullptr);
  handlers_.clear();
}

bool RuntimeImpl::OnControlThread() const noexcept { return std::this_thread::get_id() == thread_id_; }

void RuntimeImpl::Post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(cmd_mu_);
    commands_.push_back(std::move(fn));
  }
  uint64_t one = 1;
  ssize_t n = ::write(wake_.Get(), &one, sizeof(one));
  (void)n;  // EAGAIN only when the counter is saturated: already readable
}

void RuntimeImpl::Call(const std::function<void()>& fn) {
  if (OnControlThread()) {
    fn();
    return;
  }
  // Shutdown paths call this from noexcept functions: an allocation that
  // fails before the command is queued is retried, not thrown.
  for (;;) {
    try {
      std::promise<void> done;
      auto f = done.get_future();
      Post([&fn, &done] {
        fn();
        done.set_value();
      });
      f.wait();
      return;
    } catch (const std::bad_alloc&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}

bool RuntimeImpl::AddFd(int fd, uint32_t events, std::shared_ptr<EventHandler> handler, uint64_t* tag) {
  uint32_t gen = next_generation_++;
  if (next_generation_ >= 0xfffffff0u) next_generation_ = 1;
  struct epoll_event ev {};
  ev.events = events;
  ev.data.u64 = (static_cast<uint64_t>(gen) << 32) | static_cast<uint32_t>(fd);
  try {
    handlers_[fd] = Registered{gen, std::move(handler)};
  } catch (...) {  // std::bad_alloc
    return false;
  }
  if (::epoll_ctl(epoll_.Get(), EPOLL_CTL_ADD, fd, &ev) != 0) {
    handlers_.erase(fd);
    return false;
  }
  if (tag != nullptr) *tag = ev.data.u64;
  return true;
}

void RuntimeImpl::ModFd(int fd, uint64_t tag, uint32_t events) noexcept {
  struct epoll_event ev {};
  ev.events = events;
  ev.data.u64 = tag;
  ::epoll_ctl(epoll_.Get(), EPOLL_CTL_MOD, fd, &ev);
}

bool RuntimeImpl::RunAfter(uint32_t delay_ms, std::function<void()> fn) noexcept {
  try {
    timers_.emplace(NowNs() + static_cast<uint64_t>(delay_ms) * 1000000ull, std::move(fn));
    return true;
  } catch (...) {  // std::bad_alloc
    return false;
  }
}

void RuntimeImpl::RunTimers() {
  const uint64_t now = NowNs();
  while (!timers_.empty() && timers_.begin()->first <= now) {
    auto fn = std::move(timers_.begin()->second);
    timers_.erase(timers_.begin());
    fn();
  }
}

void RuntimeImpl::RemoveFd(int fd) {
  auto it = handlers_.find(fd);
  if (it == handlers_.end()) return;
  ::epoll_ctl(epoll_.Get(), EPOLL_CTL_DEL, fd, nullptr);
  // The handler may be the caller; keep it alive until the call returns.
  // (Loop() holds its own reference while dispatching, so if queuing fails
  // dropping it here is still safe.)
  auto keep = std::move(it->second.handler);
  handlers_.erase(it);
  try {
    Post([keep] {});
  } catch (...) {  // std::bad_alloc
  }
}

std::optional<uint32_t> RuntimeImpl::AllocListenerSlot() {
  std::lock_guard<std::mutex> lock(reg_mu_);
  for (uint32_t i = 0; i < kMaxListenersPerProcess; ++i) {
    if ((listener_slots_used_ & (1ull << i)) == 0) {
      listener_slots_used_ |= 1ull << i;
      return i;
    }
  }
  return std::nullopt;
}

void RuntimeImpl::FreeListenerSlot(uint32_t slot) {
  std::lock_guard<std::mutex> lock(reg_mu_);
  if (slot < kMaxListenersPerProcess) listener_slots_used_ &= ~(1ull << slot);
}

void RuntimeImpl::RegisterSubscriber(uint64_t hash, const std::shared_ptr<SubscriberImpl>& sub) {
  std::lock_guard<std::mutex> lock(reg_mu_);
  subscribers_.emplace(hash, sub);
}

void RuntimeImpl::UnregisterSubscriber(uint64_t hash, const SubscriberImpl* sub) {
  std::lock_guard<std::mutex> lock(reg_mu_);
  auto [b, e] = subscribers_.equal_range(hash);
  for (auto it = b; it != e;) {
    auto sp = it->second.lock();
    if (!sp || sp.get() == sub) {
      it = subscribers_.erase(it);
    } else {
      ++it;
    }
  }
}

void RuntimeImpl::OfferPublisher(uint64_t hash, const std::string& path) {
  std::vector<std::shared_ptr<SubscriberImpl>> subs;
  {
    std::lock_guard<std::mutex> lock(reg_mu_);
    auto [b, e] = subscribers_.equal_range(hash);
    for (auto it = b; it != e; ++it) {
      if (auto sp = it->second.lock()) subs.push_back(std::move(sp));
    }
  }
  for (auto& s : subs) s->ConnectTo(path);
}

void RuntimeImpl::ScanAll() {
  std::vector<uint64_t> hashes;
  {
    std::lock_guard<std::mutex> lock(reg_mu_);
    for (auto it = subscribers_.begin(); it != subscribers_.end(); it = subscribers_.upper_bound(it->first)) {
      hashes.push_back(it->first);
    }
  }
  for (uint64_t h : hashes) ScanFor(h);
}

void RuntimeImpl::ScanFor(uint64_t hash) {
  std::string prefix = HashPrefix(hash) + ".";
  DIR* d = ::opendir(service_dir_.c_str());
  if (d == nullptr) return;
  std::vector<std::string> paths;
  while (struct dirent* de = ::readdir(d)) {
    std::string_view name(de->d_name);
    if (name.size() > 5 && name.substr(0, prefix.size()) == prefix && name.substr(name.size() - 5) == ".sock") {
      paths.push_back(service_dir_ + "/" + std::string(name));
    }
  }
  ::closedir(d);
  for (auto& p : paths) OfferPublisher(hash, p);
}

bool RuntimeImpl::PeerAllowed(const PeerCredentials& cred) const noexcept {
  if (cred.uid == euid_ || cred.uid == 0) return true;
  return group_.has_value() && cred.gid == *group_;
}

void RuntimeImpl::HandleInotify() {
  alignas(struct inotify_event) char buf[4096];
  bool rescan = false;
  bool rewatch = false;
  for (;;) {
    ssize_t n = ::read(inotify_.Get(), buf, sizeof(buf));
    if (n <= 0) break;
    for (ssize_t off = 0; off < n;) {
      auto* ev = reinterpret_cast<struct inotify_event*>(buf + off);
      off += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);
      // Events were dropped: some publishers may never have been offered.
      if ((ev->mask & IN_Q_OVERFLOW) != 0) rescan = true;
      // The directory itself went away (rm -r, systemd RemoveIPC, tmp
      // cleaners) or was moved: nothing would ever be reported again.
      if (ev->wd == watch_ && (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) != 0) rewatch = true;
      if (ev->len == 0) continue;
      std::string_view name(ev->name);
      if (name.size() < 22 || name.substr(name.size() - 5) != ".sock" || name[16] != '.') continue;
      char* end = nullptr;
      std::string hex(name.substr(0, 16));
      uint64_t hash = std::strtoull(hex.c_str(), &end, 16);
      if (end != hex.c_str() + 16) continue;
      OfferPublisher(hash, service_dir_ + "/" + std::string(name));
    }
  }
  if (rewatch) {
    LogError("service directory '" + service_dir_ + "' was removed or moved; re-creating it");
    Rewatch(0);
  } else if (rescan) {
    LogError("inotify queue overflow; rescanning '" + service_dir_ + "'");
    ScanAll();
  }
}

void RuntimeImpl::Rewatch(uint32_t attempt) {
  if (watch_ >= 0) ::inotify_rm_watch(inotify_.Get(), watch_);  // still set after IN_MOVE_SELF
  watch_ = -1;
  if (MakeDirectory(service_dir_, group_ ? 0770 : 0700, group_)) {
    watch_ = ::inotify_add_watch(inotify_.Get(), service_dir_.c_str(), kWatchMask);
  }
  if (watch_ < 0) {
    // Retried with backoff (10 ms .. 1 s) for as long as the runtime lives.
    if (attempt == 0) LogError("cannot re-create '" + service_dir_ + "': " + std::strerror(errno) + "; retrying");
    RunAfter(std::min<uint32_t>(10u << std::min<uint32_t>(attempt, 7), 1000), [this, attempt] { Rewatch(attempt + 1); });
    return;
  }
  // Sockets created in between had no watch; publishers of this process
  // whose socket file was deleted with the directory cannot be found again
  // until they are re-created.
  ScanAll();
}

void RuntimeImpl::RunCommands() {
  uint64_t v;
  while (::read(wake_.Get(), &v, sizeof(v)) > 0) {
  }
  for (;;) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(cmd_mu_);
      if (commands_.empty()) return;
      fn = std::move(commands_.front());
      commands_.pop_front();
    }
    fn();
  }
}

void RuntimeImpl::Loop() {
  struct epoll_event events[kEpollMaxEvents];
  while (running_) {
    // No timeout unless a timer is pending (retries, quarantine polling):
    // the control thread only wakes for socket, inotify or command events,
    // so an idle process costs zero wake-ups.
    int timeout = -1;
    if (!timers_.empty()) {
      uint64_t now = NowNs();
      uint64_t at = timers_.begin()->first;
      timeout = at <= now ? 0 : static_cast<int>(std::min<uint64_t>((at - now + 999999) / 1000000, 60000));
    }
    int n = ::epoll_wait(epoll_.Get(), events, kEpollMaxEvents, timeout);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::fprintf(stderr, "[ibmw_shm] epoll_wait failed: %s\n", std::strerror(errno));
      return;
    }
    for (int i = 0; i < n && running_; ++i) {
      uint64_t tag = events[i].data.u64;
      if (tag == kWakeTag) {
        RunCommands();
      } else if (tag == kInotifyTag) {
        HandleInotify();
      } else {
        int fd = static_cast<int>(static_cast<uint32_t>(tag));
        uint32_t gen = static_cast<uint32_t>(tag >> 32);
        auto it = handlers_.find(fd);
        if (it == handlers_.end() || it->second.generation != gen) continue;
        auto handler = it->second.handler;
        handler->OnEvent(events[i].events);
      }
    }
    if (running_ && !timers_.empty()) RunTimers();
  }
}

}  // namespace detail
}  // namespace ibmw::selfdev::shm
