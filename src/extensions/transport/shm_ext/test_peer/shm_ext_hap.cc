// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// NAPI module "ibmw_shm_ext_hap" of the OHOS test HAP (IBMW_BUILD_SHM_EXT_HAP_CHECK,
// run_shm_ohos.sh hap-device). A HAP process cannot exec files, so every test
// program runs as a native child process (OH_Ability_StartNativeChildProcess):
// the child loads this library, HapChildMain loads the test library named in its
// parameters and calls its entry with an argv. Children have the uid and the
// SELinux domain of the HAP.
//
//   start(cacheDir, repeat, stepTimeout): runs the plan below on a background
//     thread; stepTimeout (seconds) bounds each EXT run, the runner gets 300 s
//     1. ext-once    EXT tests (libibmw_extensions_shm_ext_hap_test.so)
//     2. ext-repeat  the same, --gtest_repeat=<repeat> --gtest_shuffle
//     3. runner      ibmw_shm_ext_dyn_runner as a subscriber and a publisher
//                    child at the same time; both dlopen the plugin
//                    libibmw_shm_transport.so from the HAP through the core
//                    ExtensionLoader
//     Each child writes its output to <cacheDir>/<step>.log and, after its
//     entry returns, its exit code to <cacheDir>/<step>.rc. The HAP is not the
//     children's parent and cannot wait for them; it watches a pidfd instead.
//     rc=-1: timed out (killed through the pidfd), -2: ended without an exit
//     code (abort, signal), -3: wrote one but did not exit within 10 s (hangs
//     in exit), -4: no pidfd and no exit code (pidfd_open failed: HAP_STEP
//     shows pidfd_errno; ESRCH means it had already exited, anything else
//     means it was not watched and may still run).
//   poll(): "" while the plan runs, then the summary, one "HAP_STEP" line per
//     child and a final "HAP_RESULT" line (also written to <cacheDir>/summary.txt).
//
// The three EXT tests that spawn ibmw_shm_ext_peer are filtered out (exec);
// the runner covers two processes of the HAP.
//
// libchild_process.so is opened with RTLD_LOCAL in the HAP process, not linked:
// it defines its own default-visibility typeinfo for std::exception and
// std::runtime_error (bound to the system libc++.so), and as a dependency of this
// library, loaded first in every child, those copies would win the lookup of
// the test libraries over libc++_shared.so's and break every typed catch of a
// standard exception (std::terminate, catch (...) instead of
// catch (const std::exception&)).
#include <dlfcn.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "AbilityKit/native_child_process.h"
#include "napi/native_api.h"

namespace {

constexpr char kSep = '\x1f';
constexpr const char* kPeerTests =
    "-ShmDriverTest.CrossProcessPeerPublishesToDriver:ShmDriverTest.CrossProcessDriverPublishesToPeer:"
    "ShmDriverTest.ShutdownUnblocksSendToStalledRemoteConsumer";

std::string Dir(const std::string& path) { return path.substr(0, path.rfind('/')); }

std::string SelfDir() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&SelfDir), &info) == 0 || info.dli_fname == nullptr) return ".";
  return Dir(info.dli_fname);
}

std::string ReadFile(const std::string& path) {
  std::string s;
  if (FILE* f = std::fopen(path.c_str(), "r")) {
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
  }
  return s;
}

// Writes path atomically (temporary file and rename), so a reader sees all or nothing.
bool WriteFile(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (f == nullptr) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  return (std::fclose(f) == 0) && ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

// In process: a HAP cannot exec, so no `rm -rf` through std::system.
bool RemoveTree(const std::string& path) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0) return errno == ENOENT;
  return ::nftw(
             path.c_str(), [](const char* p, const struct stat*, int, FTW*) { return std::remove(p) == 0 ? 0 : -1; },
             16, FTW_DEPTH | FTW_PHYS) == 0;
}

// A new, empty directory tree: <path> and <path>/<sub> for each sub.
bool FreshTree(const std::string& path, const std::vector<std::string>& subs) {
  if (!RemoveTree(path) || ::mkdir(path.c_str(), 0700) != 0) return false;
  for (const auto& sub : subs) {
    if (::mkdir((path + "/" + sub).c_str(), 0700) != 0) return false;
  }
  return true;
}

// One child process: <library> <symbol> <log path> <env> <argv...>, joined by kSep;
// <env> is NAME=VALUE pairs separated by ';'.
struct Child {
  std::string step;
  std::string library;
  std::string symbol;
  std::vector<std::string> env;
  std::vector<std::string> argv;
  int32_t pid = 0;
  int pidfd = -1;
  int start_rc = -1;
  int pidfd_errno = 0;
};

std::string Join(const Child& c, const std::string& log) {
  std::string env;
  for (const auto& e : c.env) env += (env.empty() ? "" : ";") + e;
  std::string s = c.library + kSep + c.symbol + kSep + log + kSep + env;
  for (const auto& a : c.argv) s += kSep + a;
  return s;
}

using StartFn = Ability_NativeChildProcess_ErrCode (*)(const char*, NativeChildProcess_Args, NativeChildProcess_Options,
                                                      int32_t*);

StartFn StartFunction() {
  static StartFn fn = [] {
    void* lib = ::dlopen("libchild_process.so", RTLD_NOW | RTLD_LOCAL);
    return lib != nullptr ? reinterpret_cast<StartFn>(::dlsym(lib, "OH_Ability_StartNativeChildProcess")) : nullptr;
  }();
  return fn;
}

bool Start(Child& c, const std::string& cache) {
  const std::string log = cache + "/" + c.step;
  std::remove((log + ".log").c_str());
  std::remove((log + ".rc").c_str());
  std::string params = Join(c, log);
  NativeChildProcess_Args args{};
  args.entryParams = params.data();
  NativeChildProcess_Options options{};
  options.isolationMode = NCP_ISOLATION_MODE_NORMAL;
  const StartFn start = StartFunction();
  c.start_rc = start != nullptr ? start("libibmw_shm_ext_hap.so:HapChildMain", args, options, &c.pid) : -1;
  // Opened right away; if the child is already gone (ESRCH) its pid is not
  // used any more. Only the short window between the start call and this
  // pidfd_open can still pick up a reused pid.
  if (c.start_rc == NCP_NO_ERROR) {
    c.pidfd = static_cast<int>(::syscall(SYS_pidfd_open, c.pid, 0));
    if (c.pidfd < 0) c.pidfd_errno = errno;
  }
  return c.start_rc == NCP_NO_ERROR;
}

bool Exited(int pidfd, int timeout_ms) {
  pollfd p{pidfd, POLLIN, 0};
  return ::poll(&p, 1, timeout_ms) > 0;
}

// The child's exit code once it has exited, or the negative codes of the
// header comment. Signals go through the pidfd only, never to a bare pid.
int Wait(Child& c, const std::string& cache, std::chrono::seconds timeout) {
  const std::string rc_path = cache + "/" + c.step + ".rc";
  auto code = [&]() -> int {
    const std::string rc = ReadFile(rc_path);
    return rc.empty() ? -2 : std::atoi(rc.c_str());
  };
  if (c.pidfd < 0) {
    const std::string rc = ReadFile(rc_path);
    return rc.empty() ? -4 : std::atoi(rc.c_str());
  }
  const auto end = std::chrono::steady_clock::now() + timeout;
  int result = -1;
  for (;;) {
    if (Exited(c.pidfd, 200)) {
      result = code();  // renamed into place before the entry returned
      break;
    }
    if (!ReadFile(rc_path).empty()) {
      // Exit code written: the child must now get through its exit
      // (destructors, unloading) as well.
      result = Exited(c.pidfd, 10000) ? code() : -3;
      break;
    }
    if (std::chrono::steady_clock::now() >= end) break;
  }
  if (result == -1 || result == -3) {
    ::syscall(SYS_pidfd_send_signal, c.pidfd, SIGKILL, nullptr, 0);
    Exited(c.pidfd, 5000);
  }
  ::close(c.pidfd);
  c.pidfd = -1;
  return result;
}

std::string Owner(const void* addr) {
  Dl_info info{};
  if (addr == nullptr || dladdr(addr, &info) == 0 || info.dli_fname == nullptr) return "unresolved";
  return info.dli_fname;
}

// A memfd made as selfdev CreateSegment makes one (map with MAP_POPULATE,
// then seal against resizing), passed over a unix socketpair with
// SCM_RIGHTS, and what selfdev MapSegment checks on the received fd:
// filesystem type (TMPFS_MAGIC 0x1021994), size, seals, a shared mapping.
void ProbeMemfd(size_t size) {
  errno = 0;
  const int fd = static_cast<int>(::syscall(SYS_memfd_create, "ibmw_hap_probe", MFD_CLOEXEC | MFD_ALLOW_SEALING));
  const int tr = fd >= 0 ? ::ftruncate(fd, static_cast<off_t>(size)) : -1;
  void* m = fd >= 0 ? ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0) : MAP_FAILED;
  const int seal = fd >= 0 ? ::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) : -1;
  int sv[2] = {-1, -1};
  int rfd = -1;
  if (fd >= 0 && ::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0) {
    char byte = 0;
    iovec iov{&byte, 1};
    alignas(cmsghdr) char ctl[CMSG_SPACE(sizeof(int))] = {};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl;
    msg.msg_controllen = sizeof ctl;
    cmsghdr* c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
    if (::sendmsg(sv[0], &msg, 0) == 1) {
      alignas(cmsghdr) char rctl[CMSG_SPACE(sizeof(int))] = {};
      msghdr rmsg{};
      rmsg.msg_iov = &iov;
      rmsg.msg_iovlen = 1;
      rmsg.msg_control = rctl;
      rmsg.msg_controllen = sizeof rctl;
      if (::recvmsg(sv[1], &rmsg, MSG_CMSG_CLOEXEC) == 1 && CMSG_FIRSTHDR(&rmsg) != nullptr) {
        std::memcpy(&rfd, CMSG_DATA(CMSG_FIRSTHDR(&rmsg)), sizeof(int));
      }
    }
  }
  struct statfs sfs {};
  struct stat st {};
  const int fs = rfd >= 0 ? ::fstatfs(rfd, &sfs) : -1;
  const int sz = rfd >= 0 ? ::fstat(rfd, &st) : -1;
  const int seals = rfd >= 0 ? ::fcntl(rfd, F_GET_SEALS) : -1;
  void* rm = rfd >= 0 ? ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, rfd, 0) : MAP_FAILED;
  std::printf("HAP_MEMFD size=%zu fd=%d ftruncate=%d mmap=%d add_seals=%d received=%d fstatfs=%d f_type=0x%lx "
              "fstat=%d reg=%d st_size=%lld seals=0x%x rmmap=%d errno=%d\n",
              size, fd, tr, m != MAP_FAILED ? 1 : 0, seal, rfd, fs, static_cast<unsigned long>(sfs.f_type), sz,
              S_ISREG(st.st_mode) ? 1 : 0, static_cast<long long>(st.st_size), seals, rm != MAP_FAILED ? 1 : 0, errno);
  if (rm != MAP_FAILED) ::munmap(rm, size);
  if (m != MAP_FAILED) ::munmap(m, size);
  for (int f : {fd, rfd, sv[0], sv[1]}) {
    if (f >= 0) ::close(f);
  }
}

// The C++ runtime the child process sees, as the runner's DYN_ABI does for an
// exe and its plugin: which DSO defines each libc++abi symbol for a global
// lookup (default=), for the test library and its dependencies (lib=) and, for
// the typeinfo, the copy this library's own code is bound to (self=). Then a
// throw and a typed catch within this library (HAP_EH).
void ReportAbi(void* lib) {
  {
    // The C++ runtimes and libchild_process.so mapped in this process.
    std::string seen;
    std::string maps = ReadFile("/proc/self/maps");
    for (size_t p = 0; p < maps.size();) {
      size_t e = maps.find('\n', p);
      if (e == std::string::npos) e = maps.size();
      const std::string line = maps.substr(p, e - p);
      const size_t slash = line.find('/');
      if (slash != std::string::npos) {
        const std::string path = line.substr(slash);
        const bool runtime = path.find("libc++") != std::string::npos || path.find("child_process") != std::string::npos;
        if (runtime && seen.find(path + " ") == std::string::npos) seen += path + " ";
      }
      p = e + 1;
    }
    std::printf("HAP_MAPS %s\n", seen.c_str());
  }
  static const char* const kSymbols[] = {"__cxa_throw", "__gxx_personality_v0", "__cxa_begin_catch", "_Znwm",
                                         "_ZTISt9exception", "_ZTISt13runtime_error"};
  for (const char* sym : kSymbols) {
    std::printf("HAP_ABI symbol=%s default=%s lib=%s\n", sym, Owner(dlsym(RTLD_DEFAULT, sym)).c_str(),
                Owner(lib != nullptr ? dlsym(lib, sym) : nullptr).c_str());
  }
  std::printf("HAP_ABI self typeid(std::exception)=%s typeid(std::runtime_error)=%s\n",
              Owner(&typeid(std::exception)).c_str(), Owner(&typeid(std::runtime_error)).c_str());
  bool typed = false;
  try {
    throw std::runtime_error("hap");
  } catch (const std::exception&) {
    typed = true;
  } catch (...) {
  }
  std::printf("HAP_EH typed_catch=%d\n", typed ? 1 : 0);
  ProbeMemfd(64);
  ProbeMemfd(4096);
  {
    // selfdev MapSegment, when fstat is denied, takes "has seals" for "is a
    // shmem regular file": F_GET_SEALS must fail on other files.
    int pipe_fds[2] = {-1, -1};
    const int pipe_seals = ::pipe2(pipe_fds, O_CLOEXEC) == 0 ? ::fcntl(pipe_fds[0], F_GET_SEALS) : -2;
    for (int f : pipe_fds) {
      if (f >= 0) ::close(f);
    }
    const int dir = ::open("/data/storage/el2/base", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int dir_seals = dir >= 0 ? ::fcntl(dir, F_GET_SEALS) : -2;
    if (dir >= 0) ::close(dir);
    // A character device (devtmpfs reports TMPFS_MAGIC too) and a regular
    // file outside shmem.
    const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    const int null_seals = null_fd >= 0 ? ::fcntl(null_fd, F_GET_SEALS) : -2;
    if (null_fd >= 0) ::close(null_fd);
    char file_path[] = "/data/storage/el2/base/ibseals.XXXXXX";
    const int file_fd = ::mkstemp(file_path);
    const int file_seals = file_fd >= 0 ? ::fcntl(file_fd, F_GET_SEALS) : -2;
    if (file_fd >= 0) {
      ::close(file_fd);
      ::unlink(file_path);
    }
    std::printf("HAP_SEALS pipe=%d dir=%d devnull=%d file=%d\n", pipe_seals, dir_seals, null_seals, file_seals);
  }
  std::fflush(stdout);
}

std::mutex g_mu;
std::string g_summary;
std::atomic_bool g_started{false};

void RunPlan(std::string cache, int repeat, int step_timeout) {
  const std::string libs = SelfDir();
  // TMPDIR: the EXT tests create <TMPDIR>/ibmwx.XXXXXX/ibmw_shm.<uid>.<domain>,
  // at most 63 bytes; the sandbox cache directory is too deep for 8-digit uids.
  const std::string tmp = Dir(cache);
  std::string summary;
  bool all = true;
  auto report = [&](const Child& c, int rc) {
    char line[256];
    std::snprintf(line, sizeof line, "HAP_STEP step=%s start_rc=%d pid=%d pidfd_errno=%d rc=%d\n", c.step.c_str(),
                  c.start_rc, static_cast<int>(c.pid), c.pidfd_errno, rc);
    summary += line;
    all = all && c.start_rc == NCP_NO_ERROR && rc == 0;
  };

  const std::string test_lib = libs + "/libibmw_extensions_shm_ext_hap_test.so";
  Child once{"ext-once", test_lib, "ShmExtHapRunTests", {"TMPDIR=" + tmp}, {"ext_test", std::string("--gtest_filter=") + kPeerTests}};
  const std::chrono::seconds ext_timeout(step_timeout);
  report(once, Start(once, cache) ? Wait(once, cache, ext_timeout) : -1);
  Child rep{"ext-repeat", test_lib, "ShmExtHapRunTests", {"TMPDIR=" + tmp},
            {"ext_test", std::string("--gtest_filter=") + kPeerTests, "--gtest_repeat=" + std::to_string(repeat),
             "--gtest_shuffle"}};
  report(rep, Start(rep, cache) ? Wait(rep, cache, ext_timeout) : -1);

  // Same arguments as run_shm_ohos.sh dyn_runner_check.
  const std::string root = cache + "/r";
  if (!FreshTree(root, {"s", "sync"})) {
    summary += std::string("HAP_STEP step=runner-dirs error=") + std::strerror(errno) + "\n";
    all = false;
  }
  std::vector<std::string> args{"--plugin", libs + "/libibmw_shm_transport.so", "--sync", root + "/sync", "--topics", "20",
                                "--early",  "6", "--messages", "2000", "--cycles", "2"};
  const std::string runner_lib = libs + "/libibmw_shm_ext_hap_runner.so";
  Child sub{"runner-sub", runner_lib, "ShmExtDynRunnerMain", {"IBMW_SHM_ROOT=" + root + "/s"}, {"runner", "sub"}};
  Child pub{"runner-pub", runner_lib, "ShmExtDynRunnerMain", {"IBMW_SHM_ROOT=" + root + "/s"}, {"runner", "pub"}};
  sub.argv.insert(sub.argv.end(), args.begin(), args.end());
  pub.argv.insert(pub.argv.end(), args.begin(), args.end());
  const bool sub_ok = Start(sub, cache);
  const bool pub_ok = Start(pub, cache);
  report(pub, pub_ok ? Wait(pub, cache, std::chrono::seconds(300)) : -1);
  report(sub, sub_ok ? Wait(sub, cache, std::chrono::seconds(300)) : -1);
  if (!RemoveTree(root)) {
    summary += std::string("HAP_STEP step=runner-cleanup error=") + std::strerror(errno) + "\n";
    all = false;
  }

  summary += std::string("HAP_RESULT result=") + (all ? "PASS" : "FAIL") + "\n";
  WriteFile(cache + "/summary.txt", summary);
  std::lock_guard<std::mutex> lock(g_mu);
  g_summary = summary;
}

std::string StringArg(napi_env env, napi_value v) {
  char buf[1024] = "";
  size_t len = 0;
  napi_get_value_string_utf8(env, v, buf, sizeof buf, &len);
  return std::string(buf, len);
}

napi_value JsStart(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t repeat = 1;
  int32_t step_timeout = 0;
  if (argc < 3 || napi_get_value_int32(env, argv[1], &repeat) != napi_ok || repeat < 1 ||
      napi_get_value_int32(env, argv[2], &step_timeout) != napi_ok || step_timeout < 1) {
    napi_throw_error(env, nullptr, "start(cacheDir, repeat, stepTimeout)");
    return nullptr;
  }
  if (g_started.exchange(true)) {
    napi_throw_error(env, nullptr, "already started");
    return nullptr;
  }
  std::thread(RunPlan, StringArg(env, argv[0]), repeat, step_timeout).detach();
  return nullptr;
}

napi_value JsPoll(napi_env env, napi_callback_info) {
  std::string s;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    s = g_summary;
  }
  napi_value v;
  napi_create_string_utf8(env, s.c_str(), s.size(), &v);
  return v;
}

napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor d[] = {
      {"start", nullptr, JsStart, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"poll", nullptr, JsPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_define_properties(env, exports, sizeof d / sizeof d[0], d);
  return exports;
}

napi_module g_module = {1, 0, nullptr, Init, "ibmw_shm_ext_hap", nullptr, {nullptr}};

}  // namespace

extern "C" __attribute__((constructor)) void RegisterShmHapModule() { napi_module_register(&g_module); }

// Native child process entry: see Join() for the parameters.
extern "C" __attribute__((visibility("default"))) void HapChildMain(NativeChildProcess_Args args) {
  std::vector<std::string> parts;
  const std::string params = args.entryParams != nullptr ? args.entryParams : "";
  for (size_t p = 0;;) {
    const size_t e = params.find(kSep, p);
    parts.push_back(params.substr(p, e == std::string::npos ? std::string::npos : e - p));
    if (e == std::string::npos) break;
    p = e + 1;
  }
  if (parts.size() < 5) return;
  const std::string& log = parts[2];
  const int fd = ::open((log + ".log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd >= 0) {
    std::fflush(stdout);
    std::fflush(stderr);
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
  }
  for (size_t p = 0; p < parts[3].size();) {
    const size_t e = parts[3].find(';', p);
    const std::string kv = parts[3].substr(p, e == std::string::npos ? std::string::npos : e - p);
    const size_t eq = kv.find('=');
    if (eq != std::string::npos) ::setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
    if (e == std::string::npos) break;
    p = e + 1;
  }
  // Relative paths (the engine writes ./cfg) go to the step's directory; the
  // child starts in "/", which the HAP cannot write.
  if (::chdir(Dir(log).c_str()) != 0) std::fprintf(stderr, "HAP_CHILD chdir: %s\n", std::strerror(errno));
  std::string context = ReadFile("/proc/self/attr/current");
  context.erase(context.find_last_not_of(std::string("\n\0", 2)) + 1);
  std::printf("HAP_CHILD pid=%d context=%s entry=%s\n", static_cast<int>(::getpid()), context.c_str(), parts[1].c_str());
  std::fflush(stdout);
  int rc = 125;
  void* lib = ::dlopen(parts[0].c_str(), RTLD_NOW | RTLD_LOCAL);
  ReportAbi(lib);
  if (lib != nullptr) {
    if (auto* entry = reinterpret_cast<int (*)(int, char**)>(::dlsym(lib, parts[1].c_str()))) {
      std::vector<char*> argv;
      for (size_t i = 4; i < parts.size(); ++i) argv.push_back(parts[i].data());
      argv.push_back(nullptr);
      rc = entry(static_cast<int>(argv.size() - 1), argv.data());
    } else {
      std::fprintf(stderr, "HAP_CHILD dlsym %s: %s\n", parts[1].c_str(), ::dlerror());
    }
  } else {
    std::fprintf(stderr, "HAP_CHILD dlopen %s: %s\n", parts[0].c_str(), ::dlerror());
  }
  std::fflush(stdout);
  std::fflush(stderr);
  WriteFile(log + ".rc", std::to_string(rc) + "\n");
}
