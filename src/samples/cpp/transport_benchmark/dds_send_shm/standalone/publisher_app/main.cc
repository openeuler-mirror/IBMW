// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// dds_send_publisher_app: standalone publisher process for the
// standard non-loaned DDS Send benchmark. Pair with
// dds_send_subscriber_app (see start_dds_send.sh). No iox-roudi
// needed -- FastDDS owns its own SHM segment via DataSharing /
// SharedMemoryTransport.
//
// Unlike dds_loaned, no driver pointer needs to be injected into the
// module: the publish path goes through `ibmw::transport::Send()`,
// which the engine routes to the dds driver via the yaml
// `messaging.publishers` rule. The dds driver CDR-serializes via
// GenericDdsPubSubType<T>::serialize, then FastDDS hands the bytes to
// its transport (SHM if both peers advertise it, UDP otherwise).

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include "cpp/core.h"
#include "engine/engine.h"

#include "publisher_module/publisher_module.h"

using namespace ibmw::runtime::core;

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

int main(int argc, char** argv) {
  if (!InstallSignalHandlers()) {
    std::cerr << "Failed to set up SIGINT/SIGTERM handling." << std::endl;
    return -1;
  }

  if (argc <= 1) {
    std::cerr << "Usage: " << argv[0] << " <publisher.yaml>\n";
    return -1;
  }

  try {
    Engine engine;
    ScopedSignalCore signal_core(&engine);

    ibmw::samples::transport_benchmark::dds_send_shm::PublisherModule pub;
    engine.GetModuleBus().RegisterModule(pub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    engine.Start();
    engine.Shutdown();
  } catch (const std::exception& e) {
    std::cerr << "dds_send_shm publisher exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
