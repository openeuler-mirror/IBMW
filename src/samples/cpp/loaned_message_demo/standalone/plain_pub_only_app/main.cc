// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include "cpp/core.h"
#include "engine/engine.h"
// IBMW_R1_EXEMPT_BEGIN
#include "dds_ext/dds_transport_driver.h"
// IBMW_R1_EXEMPT_END
#include "../../managed/plain_publisher_module/plain_publisher_module.h"

using namespace ibmw::runtime::core;
using namespace ibmw::examples::loaned_message_demo;
namespace dds_ext = ibmw::extensions::dds_extension;

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

  try {
    Engine core;
    ScopedSignalCore signal_core(&core);

    PlainPublisherModule plain_pub;
    core.GetModuleBus().RegisterModule(plain_pub.NativeHandle());

    Engine::Options options;
    if (argc > 1) {
      options.cfg_file_path = argv[1];
    } else {
      std::cerr << "Usage: " << argv[0] << " <config_file_path>\n";
      return -1;
    }

    core.Initialize(options);

    // Find the DDS transport driver from the engine's used transport drivers.
    dds_ext::DdsTransportDriver* driver = nullptr;
    for (auto* td : core.GetTransportBus().GetUsedTransportDriver()) {
      if (td->Name() == "dds") {
        driver = dynamic_cast<dds_ext::DdsTransportDriver*>(td);
        break;
      }
    }
    if (driver) {
      plain_pub.SetDdsDriver(driver);
    }

    core.Start();
    core.Shutdown();
  } catch (const std::exception& e) {
    std::cerr << "Exception: " << e.what() << "\n";
    return -1;
  }

  return 0;
}
