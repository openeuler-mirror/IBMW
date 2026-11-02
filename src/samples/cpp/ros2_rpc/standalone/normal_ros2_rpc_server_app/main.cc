// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

#include "engine/engine.h"

#include "cpp/core.h"

#include "RosTestRpc.ibmw_rpc.srv.h"

using namespace ibmw::runtime::core;

class RosTestRpcSyncServiceImpl : public example_ros2::srv::RosTestRpcSyncService {
 public:
  explicit RosTestRpcSyncServiceImpl(ibmw::Runtime module_handle) : module_handle_(module_handle) {}
  ~RosTestRpcSyncServiceImpl() override = default;

  ibmw::rpc::Status RosTestRpc(
      ibmw::rpc::ContextRef ctx,
      const example_ros2::srv::RosTestRpc_Request& req,
      example_ros2::srv::RosTestRpc_Response& rsp) override {
    rsp.code = 123;

    MW_LOG(module_handle_.GetLogger(), ibmw::common::util::kLogLevelInfo,
                  "Get new service call. context: {}, req:\n{}",
                  ctx.ToString(), example_ros2::srv::to_yaml(req));

    return ibmw::rpc::Status();
  }

 private:
  ibmw::Runtime module_handle_;
};

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

  std::cout << "Ibmw start." << std::endl;

  try {
    Engine core;
    ScopedSignalCore signal_core(&core);

    // Initialize
    Engine::Options options;
    if (argc > 1) options.cfg_file_path = argv[1];

    core.Initialize(options);

    // Create Module
    ibmw::Runtime module_handle(
        core.GetModuleBus().CreateModule("NormalRpcSyncServerModule"));

    // Create service
    auto service_ptr = std::make_shared<RosTestRpcSyncServiceImpl>(module_handle);

    // Register service
    bool ret = module_handle.GetService().RegisterService(service_ptr.get());
    MW_CHECK_LOG_THROW(module_handle.GetLogger(), ret, ibmw::common::util::kLogLevelError, "Register service failed.");

    MW_LOG(module_handle.GetLogger(), ibmw::common::util::kLogLevelInfo, "Register service succeeded.");

    // Start
    core.Start();

    // Shutdown
    core.Shutdown();
  } catch (const std::exception& e) {
    std::cout << "Ibmw run with exception and exit. " << e.what()
              << std::endl;
    return -1;
  }

  std::cout << "Ibmw exit." << std::endl;

  return 0;
}
