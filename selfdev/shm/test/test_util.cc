// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "test_util.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>

#include "ibmw_selfdev/shm/runtime.h"

#ifndef IBMW_SHM_PEER_PATH
#error "IBMW_SHM_PEER_PATH must point at the ibmw_selfdev_shm_peer binary"
#endif

namespace ibmw::selfdev::shm::test {

namespace {

std::string MakeRoot() {
  const char* base = std::getenv("TMPDIR");
  std::string tmpl = std::string(base != nullptr && *base != '\0' ? base : "/tmp") + "/ibmwt.XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) std::abort();
  return buf.data();
}

void RemoveTree(const std::string& path) {
  if (DIR* d = ::opendir(path.c_str())) {
    while (struct dirent* de = ::readdir(d)) {
      std::string n = de->d_name;
      if (n == "." || n == "..") continue;
      std::string p = path + "/" + n;
      if (de->d_type == DT_DIR) {
        RemoveTree(p);
      } else {
        ::unlink(p.c_str());
      }
    }
    ::closedir(d);
  }
  ::rmdir(path.c_str());
}

class ShmEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    ::setenv("IBMW_SHM_ROOT", TestRoot().c_str(), 1);
    ::unsetenv("IBMW_SHM_DOMAIN");
    // Children killed by tests must not take the test binary down with SIGPIPE.
    ::signal(SIGPIPE, SIG_IGN);
    auto r = Runtime::Init(RuntimeOptions{});
    ASSERT_TRUE(r.ok()) << ErrorName(r.error());
  }
  void TearDown() override {
    // Stop the control thread first: it would re-create the service
    // directory it watches while the tree is being removed.
    Runtime::Shutdown();
    RemoveTree(TestRoot());
  }
};

const auto* const g_env = ::testing::AddGlobalTestEnvironment(new ShmEnvironment());

}  // namespace

const std::string& TestRoot() {
  // Death-test children re-run the binary; inherit the parent's root.
  static const std::string root = [] {
    const char* inherited = std::getenv("IBMW_SHM_TEST_ROOT");
    if (inherited != nullptr && *inherited != '\0') return std::string(inherited);
    std::string r = MakeRoot();
    ::setenv("IBMW_SHM_TEST_ROOT", r.c_str(), 1);
    return r;
  }();
  return root;
}

std::string UniqueUrl(const std::string& base) {
  static std::atomic<uint32_t> counter{0};
  return "/test/" + base + "/" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
}

bool WaitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

void FillPattern(void* payload, uint32_t size, uint64_t seq) {
  auto* p = static_cast<uint8_t*>(payload);
  if (size >= sizeof(seq)) std::memcpy(p, &seq, sizeof(seq));
  for (uint32_t i = sizeof(seq); i < size; ++i) p[i] = static_cast<uint8_t>(seq * 31 + i);
}

uint64_t CheckPattern(const void* payload, uint32_t size) {
  auto* p = static_cast<const uint8_t*>(payload);
  uint64_t seq = 0;
  if (size < sizeof(seq)) return ~0ull;
  std::memcpy(&seq, p, sizeof(seq));
  for (uint32_t i = sizeof(seq); i < size; ++i) {
    if (p[i] != static_cast<uint8_t>(seq * 31 + i)) return ~0ull;
  }
  return seq;
}

Peer SpawnPeer(const std::vector<std::string>& args, const std::vector<std::string>& env) {
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) return {};
  pid_t pid = ::fork();
  if (pid == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    for (const auto& e : env) ::putenv(const_cast<char*>(e.c_str()));
    std::vector<char*> argv;
    // Cross-testing: the build-time path does not exist on the target device.
    const char* override_path = std::getenv("IBMW_SHM_PEER_PATH");
    std::string path = override_path != nullptr ? override_path : IBMW_SHM_PEER_PATH;
    argv.push_back(path.data());
    std::vector<std::string> copy = args;
    for (auto& a : copy) argv.push_back(a.data());
    argv.push_back(nullptr);
    ::execv(path.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(fds[1]);
  return Peer{pid, fds[0]};
}

std::string ReadLine(Peer& p, std::chrono::milliseconds timeout) {
  std::string line;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    struct pollfd pfd {p.out, POLLIN, 0};
    int left = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if (::poll(&pfd, 1, std::max(left, 1)) <= 0) continue;
    char c;
    if (::read(p.out, &c, 1) != 1) return line;
    if (c == '\n') return line;
    line.push_back(c);
  }
  return line;
}

int WaitPeer(Peer& p, std::chrono::milliseconds timeout) {
  if (p.pid <= 0) return -1;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  for (;;) {
    pid_t r = ::waitpid(p.pid, &status, WNOHANG);
    if (r == p.pid) break;
    if (std::chrono::steady_clock::now() >= deadline) {
      ::kill(p.pid, SIGKILL);
      ::waitpid(p.pid, &status, 0);
      p.pid = -1;
      ::close(p.out);
      return -1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  p.pid = -1;
  ::close(p.out);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

void KillPeer(Peer& p, int sig) {
  if (p.pid > 0) ::kill(p.pid, sig);
}

int64_t ThreadSwitches(const std::string& comm) {
  DIR* d = ::opendir("/proc/self/task");
  if (d == nullptr) return -1;
  int64_t result = -1;
  while (struct dirent* de = ::readdir(d)) {
    if (de->d_name[0] == '.') continue;
    std::string base = std::string("/proc/self/task/") + de->d_name;
    std::ifstream c(base + "/comm");
    std::string name;
    std::getline(c, name);
    if (name != comm) continue;
    std::ifstream st(base + "/status");
    std::string line;
    int64_t total = 0;
    while (std::getline(st, line)) {
      if (line.rfind("voluntary_ctxt_switches:", 0) == 0 || line.rfind("nonvoluntary_ctxt_switches:", 0) == 0) {
        total += std::stoll(line.substr(line.find(':') + 1));
      }
    }
    result = total;
    break;
  }
  ::closedir(d);
  return result;
}

}  // namespace ibmw::selfdev::shm::test
