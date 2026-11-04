// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>

#include "cpp/core.h"
#include "engine/engine.h"

// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END
#include "subscriber_module/subscriber_module.h"

using namespace ibmw::runtime::core;
namespace iox_ext = ibmw::extensions::iceoryx_extension;

// SIGINT/SIGTERM only write the signal number to a pipe (async-signal-safe);
// main blocks in read() on the other end instead of polling a flag.
int g_signal_pipe[2] = {-1, -1};

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
  return std::signal(SIGINT, SignalHandler) != SIG_ERR &&
         std::signal(SIGTERM, SignalHandler) != SIG_ERR;
}

// Blocks until SIGINT or SIGTERM has been received.
void WaitForShutdownSignal() {
  unsigned char sig = 0;
  while (true) {
    const ssize_t n = read(g_signal_pipe[0], &sig, 1);
    if (n == 1) return;
    if (n < 0 && errno == EINTR) continue;
    std::cerr << "signal pipe read failed: " << std::strerror(errno) << "\n";
    return;
  }
}

int main(int argc, char** argv) {
  if (!InstallSignalHandlers()) {
    std::cerr << "Failed to set up SIGINT/SIGTERM handling." << std::endl;
    return -1;
  }

  if (argc <= 1) {
    std::cerr << "Usage: " << argv[0] << " <subscriber.yaml>\n";
    return -1;
  }

  try {
    Engine engine;
    ibmw::samples::transport_benchmark::iox_loaned_ctx::SubscriberModule sub;
    engine.GetModuleBus().RegisterModule(sub.NativeHandle());

    Engine::Options options;
    options.cfg_file_path = argv[1];
    engine.Initialize(options);

    iox_ext::IceoryxTransportDriver* driver = nullptr;
    for (auto* td : engine.GetTransportBus().GetUsedTransportDriver()) {
      if (td->Name() == "iceoryx") {
#if defined(__OHOS__) || defined(__OHOS)
        // OHOS hides RTTI identity across this plugin boundary.
        driver = static_cast<iox_ext::IceoryxTransportDriver*>(td);
#else
        driver = dynamic_cast<iox_ext::IceoryxTransportDriver*>(td);
#endif
        break;
      }
    }
    if (driver == nullptr) {
      std::cerr << "iceoryx driver not found in TransportBus\n";
      return -1;
    }
    sub.SetDriver(driver);

    std::cout << "PERF_SUBSCRIBER_READY" << std::endl;
    auto stopped = engine.AsyncStart();
    WaitForShutdownSignal();
    engine.Shutdown();
    stopped.get();
  } catch (const std::exception& e) {
    std::cerr << "iox_loaned_ctx subscriber exception: " << e.what() << "\n";
    return -1;
  }
  return 0;
}
