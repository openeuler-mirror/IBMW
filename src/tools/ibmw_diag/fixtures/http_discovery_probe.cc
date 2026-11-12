// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "engine/discovery/service_discovery.h"
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <nlohmann/json.hpp>

namespace {
size_t Count(const char* path) {
  return static_cast<size_t>(std::distance(std::filesystem::directory_iterator(path),
                                         std::filesystem::directory_iterator{}));
}
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}
int main(int argc, char** argv) {
  try {
    if (argc != 7) throw std::runtime_error("Expected plugin-dir registry endpoint timeout-ms iterations success|timeout");
    setenv("IBMW_DISCOVERY_PLUGIN_DIR", argv[1], 1);
    const bool success = std::string(argv[6]) == "success";
    using namespace ibmw::discovery;
    const std::vector<ServiceDiscoveryOptions> sources{
        {.registry_path = argv[2]}, {.backend = ServiceDiscoveryBackend::kHttp, .endpoint = argv[3]}};
    const auto timeout = std::chrono::milliseconds(std::stoi(argv[4]));
    const auto iterations = std::stoi(argv[5]);
    nlohmann::json elapsed = nlohmann::json::array();
    const auto fds = Count("/proc/self/fd");
    const auto threads = Count("/proc/self/task");
    for (int i = 0; i < iterations; ++i) {
      const auto start = std::chrono::steady_clock::now();
      const auto result = DiscoverServices(sources, timeout);
      elapsed.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
      Require(result.sources.size() == 2, "Missing source status");
      Require(result.sources[0].status == DiscoveryStatus::kReady, "Registry did not remain ready");
      Require(!result.services.empty() && result.services[0].advertisement.service_name == "registry-regression",
              "Ready registry service was lost");
      if (success) {
        Require(result.Succeeded(), "HTTP discovery failed");
        Require(result.services.size() == 2 && result.services[1].advertisement.service_name == "dns-regression",
                "Actual service was not discovered");
      } else {
        Require(!result.Succeeded() && result.sources[1].message.find("timed out") != std::string::npos,
                "Expected a discovery timeout");
      }
      int status = 0;
      Require(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "DNS helper survived or was not reaped");
      Require(Count("/proc/self/fd") == fds, "File descriptor count grew");
      // /proc can briefly retain a kernel task after the future has joined.
      // Require return to the pre-query count, with bounded teardown slack.
      const auto settled = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      while (Count("/proc/self/task") != threads && std::chrono::steady_clock::now() < settled)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      Require(Count("/proc/self/task") == threads, "Thread count grew");
    }
    std::cout << nlohmann::json{{"elapsed_ms", elapsed}, {"fds", fds}, {"threads", threads}} << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
