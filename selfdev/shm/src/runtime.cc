// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>

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

constexpr uint32_t kEpollMaxEvents = 64;
// epoll user data: generation in the high half, fd in the low half, so an
// event for a closed fd whose number was reused is ignored.
constexpr uint64_t kWakeTag = 0xffffffff00000000ull;
constexpr uint64_t kInotifyTag = 0xfffffffe00000000ull;

}  // namespace

RuntimeImpl* RuntimeImpl::Instance() noexcept {
  {
    std::lock_guard<std::mutex> lock(g_instance_mu);
    if (g_instance != nullptr) return g_instance;
  }
  (void)Init(RuntimeOptions{});
  std::lock_guard<std::mutex> lock(g_instance_mu);
  return g_instance;
}

Result<void> RuntimeImpl::Init(const RuntimeOptions& options) {
  std::lock_guard<std::mutex> lock(g_instance_mu);
  if (g_instance != nullptr) {
    if (ResolveServiceDir(options) == g_instance->ServiceDir()) return {};
    return Error::kAlreadyInitialized;
  }
  auto* rt = new RuntimeImpl();
  auto r = rt->Start(options);
  if (!r) {
    delete rt;
    return r;
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

std::string RuntimeImpl::ResolveServiceDir(const RuntimeOptions& options) {
  std::string root = options.root_dir;
  if (root.empty()) {
    const char* env = std::getenv("IBMW_SHM_ROOT");
    root = (env != nullptr && *env != '\0') ? env : "/tmp";
  }
  std::string domain = options.domain.empty() ? "default" : options.domain;
  if (const char* env = std::getenv("IBMW_SHM_DOMAIN"); env != nullptr && *env != '\0') domain = env;
  if (domain.find('/') != std::string::npos || domain.size() > 32) return {};
  std::string owner = options.group ? "g" + std::to_string(*options.group) : std::to_string(::geteuid());
  return root + "/ibmw_shm." + owner + "." + domain;
}

Result<void> RuntimeImpl::Start(const RuntimeOptions& options) {
  options_ = options;
  group_ = options.group;
  euid_ = ::geteuid();
  service_dir_ = ResolveServiceDir(options);
  if (service_dir_.empty()) return Error::kInvalidArg;
  if (!MakeDirectory(service_dir_, group_ ? 0770 : 0700, group_)) return Error::kPermission;

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
  if (::inotify_add_watch(inotify_.Get(), service_dir_.c_str(), IN_MOVED_TO) < 0) return Error::kSystem;

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
  std::promise<void> done;
  auto f = done.get_future();
  Post([&fn, &done] {
    fn();
    done.set_value();
  });
  f.wait();
}

bool RuntimeImpl::AddFd(int fd, uint32_t events, std::shared_ptr<EventHandler> handler) {
  uint32_t gen = next_generation_++;
  if (next_generation_ >= 0xfffffff0u) next_generation_ = 1;
  struct epoll_event ev {};
  ev.events = events;
  ev.data.u64 = (static_cast<uint64_t>(gen) << 32) | static_cast<uint32_t>(fd);
  if (::epoll_ctl(epoll_.Get(), EPOLL_CTL_ADD, fd, &ev) != 0) return false;
  handlers_[fd] = Registered{gen, std::move(handler)};
  return true;
}

void RuntimeImpl::RemoveFd(int fd) {
  auto it = handlers_.find(fd);
  if (it == handlers_.end()) return;
  ::epoll_ctl(epoll_.Get(), EPOLL_CTL_DEL, fd, nullptr);
  // The handler may be the caller; keep it alive until the call returns.
  auto keep = std::move(it->second.handler);
  handlers_.erase(it);
  Post([keep] {});
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
  for (;;) {
    ssize_t n = ::read(inotify_.Get(), buf, sizeof(buf));
    if (n <= 0) return;
    for (ssize_t off = 0; off < n;) {
      auto* ev = reinterpret_cast<struct inotify_event*>(buf + off);
      off += static_cast<ssize_t>(sizeof(struct inotify_event) + ev->len);
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
    // No timeout: the control thread only wakes for socket, inotify or
    // command events, so an idle process costs zero wake-ups.
    int n = ::epoll_wait(epoll_.Get(), events, kEpollMaxEvents, -1);
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
  }
}

}  // namespace detail
}  // namespace ibmw::selfdev::shm
