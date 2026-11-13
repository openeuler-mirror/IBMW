// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// iceoryx_interproc_subscriber_app: standalone subscriber process for the
// two-process Iceoryx demo. Start this BEFORE the publisher so the
// iceoryx Listener has its callback wired by the time the first sample
// arrives (otherwise the early samples are dropped by RouDi history depth).

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include "cpp/core.h"
#include "engine/engine.h"
#include "subscriber_module/subscriber_module.h"

using namespace ibmw::runtime::core;

// shm_interproc_demo builds this file unchanged over the self-developed shm transport
// and overrides the two banner strings below (see shm_interproc_demo/CMakeLists.txt).
#ifndef IBMW_DEMO_TITLE
#define IBMW_DEMO_TITLE "IBMW Iceoryx Interproc Demo"
#endif
#ifndef IBMW_DEMO_TRANSPORT_NOTE
#define IBMW_DEMO_TRANSPORT_NOTE "NOTE: requires iox-roudi daemon to be running."
#endif

// SIGINT/SIGTERM only write the signal number to a pipe (async-signal-safe);
// a watcher thread calls Engine::Shutdown(), which logs and locks.
std::mutex g_core_mutex;
Engine* global_core_ptr = nullptr;
int g_signal_pipe[2] = {-1, -1};

// Makes `core` the target of SIGINT/SIGTERM while in scope.
class ScopedSignalCore {
 public:
  explicit ScopedSignalCore(Engine* core) { Set(core); }
  ~ScopedSignalCore() { Set(nullptr); }
  ScopedSignalCore(const ScopedSignalCore&) = delete;
  ScopedSignalCore& operator=(const ScopedSignalCore&) = delete;

 private:
  static void Set(Engine* core) {
    std::lock_guard<std::mutex> lck(g_core_mutex);
    global_core_ptr = core;
  }
};

void SignalHandler(int sig) {
  const int saved_errno = errno;
  const unsigned char byte = static_cast<unsigned char>(sig);
  [[maybe_unused]] auto ret = write(g_signal_pipe[1], &byte, 1);
  errno = saved_errno;
}

bool InstallSignalHandlers() {
  if (pipe2(g_signal_pipe, O_CLOEXEC) != 0 ||
      fcntl(g_signal_pipe[1], F_SETFL, O_NONBLOCK) != 0) {
    return false;
  }
  std::thread([] {
    unsigned char sig = 0;
    while (true) {
      const ssize_t n = read(g_signal_pipe[0], &sig, 1);
      if (n != 1) {
        if (n < 0 && errno == EINTR) continue;
        return;
      }
      std::lock_guard<std::mutex> lck(g_core_mutex);
      if (global_core_ptr == nullptr) {
        // No engine (anymore): die from the signal.
        std::signal(sig, SIG_DFL);
        raise(sig);
        continue;
      }
      global_core_ptr->Shutdown();
    }
  }).detach();
  return std::signal(SIGINT, SignalHandler) != SIG_ERR &&
         std::signal(SIGTERM, SignalHandler) != SIG_ERR;
}

int32_t main(int32_t argc, char** argv) {
  if (!InstallSignalHandlers()) {
    std::cerr << "Failed to set up SIGINT/SIGTERM handling." << std::endl;
    return -1;
  }

  std::cout << IBMW_DEMO_TITLE " (Subscriber) start." << std::endl;
  std::cout << IBMW_DEMO_TRANSPORT_NOTE << std::endl;

  try {
    Engine core;
    ScopedSignalCore signal_core(&core);

    iceoryx_interproc_demo::SubscriberModule subscriber_module;
    core.GetModuleBus().RegisterModule(subscriber_module.NativeHandle());

    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config_file_path>" << std::endl;
      return -1;
    }

    core.Initialize(options);

    std::cout << "Starting IBMW Engine (subscriber)..." << std::endl;
    core.Start();

    core.Shutdown();
  } catch (const std::exception& e) {
    std::cout << "IBMW subscriber exception: " << e.what() << std::endl;
    return -1;
  }

  std::cout << IBMW_DEMO_TITLE " (Subscriber) exit." << std::endl;
  return 0;
}
