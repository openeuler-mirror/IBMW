// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Child process for multi-process tests. Talks to the test through stdout
// lines ("ready", "done <n>") and its exit code:
//   0 ok, 2 data mismatch, 3 timeout, 4 setup failure, 64 usage.
//
//   pub      <url> <count> <lossless> <size>  publish once a subscriber is there
//   sub      <url> <count> <lossless> <size>  receive and verify, in order if lossless
//   sub_hold <url> <n>                        take n chunks, keep them, wait to be killed
//   pub_hold <url>                            publish one chunk, wait to be killed
//   sub_write <url>                           write into a received chunk (must die by SIGSEGV)
//   sub_hold_drop <url> <n>                   like sub_hold, then shut its control sockets down
//                                             without an orderly close and wait to be killed
//   sub_hold_drop_release <url> <n>           like sub_hold_drop, then release everything on
//                                             SIGUSR1, print "released" and wait to be killed
//   probe <url> [domain|-] [root|-]           Init() with these options (if given), then print
//                                             "init <error>", "dir <service dir>", "pub <error>",
//                                             "sub <error>" ("ok" on success), "sock <mode> <gid>"
//                                             of the publisher's socket file
//   runtime_shutdown_oom <url> <n>            create n publishers, then Runtime::Shutdown() with
//                                             every allocation failing; print "done"
//   as <uid> <gid> <groups|-> <mode> ...      run a mode above with these ids (needs root);
//                                             groups is a comma-separated gid list

#include <dirent.h>
#include <grp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ibmw_selfdev/shm/shm.h"
#include "test_util.h"

using namespace ibmw::selfdev::shm;  // NOLINT

namespace {

void Say(const std::string& s) {
  std::printf("%s\n", s.c_str());
  std::fflush(stdout);
}

void Fill(void* payload, uint32_t size, uint64_t seq) {
  auto* p = static_cast<uint8_t*>(payload);
  std::memcpy(p, &seq, sizeof(seq));
  for (uint32_t i = sizeof(seq); i < size; ++i) p[i] = static_cast<uint8_t>(seq * 31 + i);
}

bool Check(const void* payload, uint32_t size, uint64_t* seq) {
  auto* p = static_cast<const uint8_t*>(payload);
  std::memcpy(seq, p, sizeof(*seq));
  for (uint32_t i = sizeof(*seq); i < size; ++i) {
    if (p[i] != static_cast<uint8_t>(*seq * 31 + i)) return false;
  }
  return true;
}

template <class Pred>
bool Wait(Pred pred, int ms) {
  for (int i = 0; i < ms; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred();
}

[[noreturn]] void Park() {
  for (;;) ::pause();
}

int RunPub(const std::string& url, uint64_t count, bool lossless, uint32_t size) {
  PublisherOptions po;
  po.too_slow = lossless ? TooSlowPolicy::kWaitForConsumer : TooSlowPolicy::kDiscardOldest;
  Publisher pub(url, po);
  if (!pub.Valid()) return 4;
  Say("ready");
  if (!Wait([&] { return pub.HasSubscribers(); }, 10000)) return 3;
  for (uint64_t seq = 1; seq <= count; ++seq) {
    auto loan = pub.Loan(size);
    if (!loan) {
      std::fprintf(stderr, "loan failed: %s\n", ErrorName(loan.error()));
      return 4;
    }
    Fill(*loan, size, seq);
    pub.Publish(*loan);
  }
  Say("done " + std::to_string(count));
  // Keep the publisher alive until the test reads everything or kills us.
  std::this_thread::sleep_for(std::chrono::seconds(30));
  return 0;
}

int RunSub(const std::string& url, uint64_t count, bool lossless, uint32_t size) {
  SubscriberOptions so;
  so.queue_capacity = lossless ? 8 : 256;
  so.queue_full = lossless ? QueueFullPolicy::kBlockProducer : QueueFullPolicy::kDiscardOldest;
  Subscriber sub(url, so);
  if (!sub.Valid()) return 4;
  Say("ready");
  uint64_t got = 0, last = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (lossless ? got < count : last < count) {
    if (std::chrono::steady_clock::now() > deadline) {
      std::fprintf(stderr, "timeout: got %llu last %llu\n", static_cast<unsigned long long>(got),
                   static_cast<unsigned long long>(last));
      return 3;
    }
    auto t = sub.Take();
    if (!t) {
      std::this_thread::yield();
      continue;
    }
    uint64_t seq = 0;
    bool ok = ChunkView::FromUserPayload(*t).PayloadSize() == size && Check(*t, size, &seq);
    sub.Release(*t);
    if (!ok || seq <= last || (lossless && seq != last + 1)) {
      std::fprintf(stderr, "mismatch: seq %llu last %llu\n", static_cast<unsigned long long>(seq),
                   static_cast<unsigned long long>(last));
      return 2;
    }
    last = seq;
    ++got;
  }
  Say("done " + std::to_string(got));
  return 0;
}

int RunSubHold(const std::string& url, uint32_t n) {
  SubscriberOptions so;
  so.queue_capacity = 16;
  Subscriber sub(url, so);
  if (!sub.Valid()) return 4;
  uint32_t held = 0;
  if (!Wait([&] {
        while (held < n && sub.Take()) ++held;  // never released
        return held == n;
      }, 10000)) {
    return 3;
  }
  Say("ready");
  Park();
}

int RunPubHold(const std::string& url) {
  Publisher pub(url);
  if (!pub.Valid()) return 4;
  if (!Wait([&] { return pub.HasSubscribers(); }, 10000)) return 3;
  auto loan = pub.Loan(64);
  if (!loan) return 4;
  Fill(*loan, 64, 1);
  pub.Publish(*loan);
  Say("ready");
  Park();
}

// Simulates control-socket trouble under a live subscriber: every
// SOCK_SEQPACKET socket of this process (only the shm connections) is shut
// down, the held chunks stay mapped.
int RunSubHoldDrop(const std::string& url, uint32_t n, bool release) {
  // Before any thread exists: all of them inherit the mask, so sigwait()
  // below is the only taker.
  sigset_t usr1;
  sigemptyset(&usr1);
  sigaddset(&usr1, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &usr1, nullptr);
  SubscriberOptions so;
  so.queue_capacity = 16;
  Subscriber sub(url, so);
  if (!sub.Valid()) return 4;
  std::vector<const void*> held;
  if (!Wait([&] {
        while (held.size() < n) {  // not released here
          auto t = sub.Take();
          if (!t) break;
          held.push_back(*t);
        }
        return held.size() == n;
      }, 10000)) {
    return 3;
  }
  if (DIR* d = ::opendir("/proc/self/fd")) {
    while (auto* de = ::readdir(d)) {
      int fd = std::atoi(de->d_name);
      int type = 0;
      socklen_t len = sizeof(type);
      if (de->d_name[0] != '.' && ::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0 && type == SOCK_SEQPACKET) {
        ::shutdown(fd, SHUT_RDWR);
      }
    }
    ::closedir(d);
  }
  if (!Wait([&] { return sub.Stats().connections == 0; }, 10000)) return 3;
  Say("ready");
  if (release) {
    int sig = 0;
    sigwait(&usr1, &sig);
    for (const void* p : held) sub.Release(p);
    Say("released");
  }
  Park();
}

int RunProbe(const std::string& url, const std::string& domain, const std::string& root) {
  if (domain != "-" || root != "-") {
    RuntimeOptions ro;
    if (domain != "-") ro.domain = domain;
    if (root != "-") ro.root_dir = root;
    auto r = Runtime::Init(ro);
    Say(std::string("init ") + ErrorName(r.ok() ? Error::kOk : r.error()));
  } else {
    Say("init -");
  }
  Say("dir " + Runtime::ServiceDirectory());
  Publisher pub(url);
  Say(std::string("pub ") + ErrorName(pub.InitError()));
  Subscriber sub(url);
  Say(std::string("sub ") + ErrorName(sub.InitError()));
  std::string sock = "none";
  if (pub.Valid()) {
    const std::string dir = Runtime::ServiceDirectory();
    if (DIR* d = ::opendir(dir.c_str())) {
      while (auto* de = ::readdir(d)) {
        const std::string name = de->d_name;
        struct stat st {};
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".sock") == 0 &&
            ::stat((dir + "/" + name).c_str(), &st) == 0) {
          char line[48];
          std::snprintf(line, sizeof(line), "%o %u", static_cast<unsigned>(st.st_mode & 07777),
                        static_cast<unsigned>(st.st_gid));
          sock = line;
        }
      }
      ::closedir(d);
    }
  }
  Say("sock " + sock);
  return 0;
}

int RunRuntimeShutdownOom(const std::string& url, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    Publisher pub(url);
    if (!pub.Valid()) return 4;
  }
  test::FailAllocations(INT_MAX);
  Runtime::Shutdown();
  test::FailAllocations(0);
  Say("done");
  return 0;
}

// Takes the ids of "as <uid> <gid> <groups|->" for good; false on failure.
bool BecomeUser(const char* uid_arg, const char* gid_arg, const char* groups_arg) {
  std::vector<gid_t> groups;
  if (std::strcmp(groups_arg, "-") != 0) {
    for (const char* p = groups_arg; *p != '\0';) {
      char* end = nullptr;
      groups.push_back(static_cast<gid_t>(std::strtoul(p, &end, 10)));
      p = *end == ',' ? end + 1 : end;
    }
  }
  const auto uid = static_cast<uid_t>(std::strtoul(uid_arg, nullptr, 10));
  const auto gid = static_cast<gid_t>(std::strtoul(gid_arg, nullptr, 10));
  return ::setgroups(groups.size(), groups.data()) == 0 && ::setresgid(gid, gid, gid) == 0 &&
         ::setresuid(uid, uid, uid) == 0 && ::geteuid() == uid && ::getegid() == gid;
}

int RunSubWrite(const std::string& url) {
  Subscriber sub(url);
  if (!sub.Valid()) return 4;
  const void* payload = nullptr;
  if (!Wait([&] {
        auto t = sub.Take();
        if (t) payload = *t;
        return t.ok();
      }, 10000)) {
    return 3;
  }
  const_cast<volatile uint8_t*>(static_cast<const uint8_t*>(payload))[0] = 1;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 5 && std::strcmp(argv[1], "as") == 0) {
    if (!BecomeUser(argv[2], argv[3], argv[4])) {
      std::perror("as");
      return 4;
    }
    argc -= 4;
    argv += 4;
  }
  if (argc < 3) return 64;
  std::string mode = argv[1];
  std::string url = argv[2];
  auto arg = [&](int i, uint64_t def) { return argc > i ? std::strtoull(argv[i], nullptr, 10) : def; };
  if (mode == "pub") return RunPub(url, arg(3, 100), arg(4, 0) != 0, static_cast<uint32_t>(arg(5, 64)));
  if (mode == "sub") return RunSub(url, arg(3, 100), arg(4, 0) != 0, static_cast<uint32_t>(arg(5, 64)));
  if (mode == "sub_hold") return RunSubHold(url, static_cast<uint32_t>(arg(3, 1)));
  if (mode == "pub_hold") return RunPubHold(url);
  if (mode == "sub_write") return RunSubWrite(url);
  if (mode == "sub_hold_drop") return RunSubHoldDrop(url, static_cast<uint32_t>(arg(3, 1)), false);
  if (mode == "sub_hold_drop_release") return RunSubHoldDrop(url, static_cast<uint32_t>(arg(3, 1)), true);
  if (mode == "runtime_shutdown_oom") return RunRuntimeShutdownOom(url, static_cast<uint32_t>(arg(3, 0)));
  if (mode == "probe") return RunProbe(url, argc > 3 ? argv[3] : "-", argc > 4 ? argv[4] : "-");
  return 64;
}
