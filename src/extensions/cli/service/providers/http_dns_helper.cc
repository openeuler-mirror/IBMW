// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Private helper: stdout is a bounded list of numeric IP addresses, one per line.
#include <iostream>
#include <memory>
#include <string>
#include <netdb.h>
#include <sys/socket.h>

int main(int argc, char** argv) {
  if (argc != 2) return 1;
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* raw = nullptr;
  if (getaddrinfo(argv[1], nullptr, &hints, &raw) != 0) return 2;
  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(raw, freeaddrinfo);
  std::string reply;
  size_t count = 0;
  for (auto* entry = raw; entry; entry = entry->ai_next) {
    if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6) continue;
    char host[NI_MAXHOST];
    if (getnameinfo(entry->ai_addr, entry->ai_addrlen, host, sizeof(host), nullptr, 0, NI_NUMERICHOST))
      return 3;
    reply += std::string(host) + '\n';
    if (++count > 128 || reply.size() > 16384) return 4;
  }
  if (reply.empty()) return 5;
  std::cout << reply << std::flush;
  return std::cout ? 0 : 6;
}
