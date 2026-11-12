// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Test-only fault injection. All other names use the real system resolver.
#include <chrono>
#include <cstring>
#include <thread>
#include <dlfcn.h>
#include <netdb.h>

extern "C" __attribute__((visibility("default"))) int getaddrinfo(const char* host, const char* service,
                           const addrinfo* hints, addrinfo** result) {
  if (host && std::strcmp(host, "slow-pr12.invalid") == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    return EAI_AGAIN;
  }
  if (host && std::strcmp(host, "budget-pr12.invalid") == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    host = "127.0.0.1";
  }
  const auto real = reinterpret_cast<decltype(&getaddrinfo)>(dlsym(RTLD_NEXT, "getaddrinfo"));
  return real ? real(host, service, hints, result) : EAI_SYSTEM;
}
