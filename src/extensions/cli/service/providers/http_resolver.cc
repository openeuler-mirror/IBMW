// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "http_resolver.h"

#include <algorithm>
#include <cstdint>
#include <cerrno>
#include <climits>
#include <csignal>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ibmw::discovery {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxReply = 16384;

class Fd {
 public:
  explicit Fd(int value) : value_(value) {
    if (value < 0) throw std::system_error(errno, std::generic_category(), "DNS pipe");
  }
  ~Fd() { Close(); }
  void Close() { if (value_ >= 0) close(value_); value_ = -1; }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int Get() const { return value_; }
 private:
  int value_;
};

class Child {
 public:
  explicit Child(pid_t pid) : pid_(pid) {}
  ~Child() {
    if (pid_ <= 0) return;
    // Never wait for getaddrinfo to return. Kill first, then reap our own child
    // before unloading this DSO. As with socket cancellation, OS scheduling is
    // outside the deadline guarantee; no DNS worker survives the query.
    kill(pid_, SIGKILL);
    while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  bool Finished() {
    int status = 0;
    const auto result = waitpid(pid_, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) return false;
    pid_ = -1;
    if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
      throw std::runtime_error("HTTP service discovery name resolution failed.");
    return true;
  }
 private:
  pid_t pid_;
};

class SpawnActions {
 public:
  SpawnActions() { Check(posix_spawn_file_actions_init(&actions_)); }
  ~SpawnActions() { posix_spawn_file_actions_destroy(&actions_); }
  posix_spawn_file_actions_t* Get() { return &actions_; }
  static void Check(int error) {
    if (error) throw std::system_error(error, std::generic_category(), "DNS helper spawn");
  }
 private:
  posix_spawn_file_actions_t actions_;
};

int RemainingMs(Clock::time_point deadline) {
  const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
  if (left <= 0) throw std::runtime_error("HTTP service discovery timed out.");
  return static_cast<int>(std::min<int64_t>(left, INT_MAX));
}
}  // namespace

std::vector<boost::asio::ip::tcp::endpoint> ResolveHttpEndpoints(
    const std::string& host, const std::string& port, Clock::time_point deadline) {
  RemainingMs(deadline);
  const auto number = static_cast<unsigned short>(std::stoul(port));
  boost::system::error_code error;
  const auto numeric = boost::asio::ip::make_address(host, error);
  if (!error) return {{numeric, number}};

  Dl_info location{};
  if (!dladdr(reinterpret_cast<const void*>(&ResolveHttpEndpoints), &location) || !location.dli_fname)
    throw std::runtime_error("Cannot locate the HTTP DNS helper.");
  const auto executable = (std::filesystem::path(location.dli_fname).parent_path() /
                           "ibmw_service_dns_helper").string();
  int pipe_fds[2];
  // Duplicate above stderr so the spawn actions also work with closed stdio.
  // Both ends are close-on-exec, including while other queries spawn helpers.
  // pipe2 makes creation atomic with respect to concurrent exec.
  if (pipe2(pipe_fds, O_CLOEXEC) != 0)
    throw std::system_error(errno, std::generic_category(), "DNS pipe");
  Fd read_original(pipe_fds[0]);
  Fd write_original(pipe_fds[1]);
  Fd input(fcntl(read_original.Get(), F_DUPFD_CLOEXEC, 3));
  std::string reply;
  {
    Fd output(fcntl(write_original.Get(), F_DUPFD_CLOEXEC, 3));
    read_original.Close();
    write_original.Close();
    SpawnActions actions;
    SpawnActions::Check(posix_spawn_file_actions_adddup2(actions.Get(), output.Get(), STDOUT_FILENO));
    SpawnActions::Check(posix_spawn_file_actions_addclose(actions.Get(), output.Get()));
    char* argv[] = {const_cast<char*>(executable.c_str()), const_cast<char*>(host.c_str()), nullptr};
    pid_t pid = -1;
    SpawnActions::Check(posix_spawn(&pid, executable.c_str(), actions.Get(), nullptr, argv, environ));
    Child child(pid);
    output.Close();
    // Both output and process exit must complete within the same deadline.
    if (fcntl(input.Get(), F_SETFL, O_NONBLOCK) < 0)
      throw std::system_error(errno, std::generic_category(), "DNS pipe flags");
    bool finished = false;
    while (true) {
      const auto remaining = RemainingMs(deadline);
      if (!finished) finished = child.Finished();
      char buffer[1024];
      const auto count = read(input.Get(), buffer, sizeof(buffer));
      if (count > 0) {
        reply.append(buffer, static_cast<size_t>(count));
        if (reply.size() > kMaxReply) throw std::runtime_error("DNS reply exceeds size limit.");
        continue;
      }
      if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        throw std::system_error(errno, std::generic_category(), "DNS pipe read");
      if (count < 0 && errno == EINTR) continue;
      if (finished) break;
      pollfd descriptor{input.Get(), POLLIN, 0};
      // EOF can precede waitable exit by a scheduling tick.
      if (poll(count == 0 ? nullptr : &descriptor, count == 0 ? 0 : 1,
               count == 0 ? std::min(remaining, 1) : remaining) < 0 && errno != EINTR)
        throw std::system_error(errno, std::generic_category(), "DNS pipe poll");
    }
  }
  std::vector<boost::asio::ip::tcp::endpoint> endpoints;
  size_t offset = 0;
  while (offset < reply.size()) {
    const auto end = reply.find('\n', offset);
    if (end == std::string::npos || endpoints.size() >= 128)
      throw std::runtime_error("Invalid DNS helper reply.");
    const auto address = boost::asio::ip::make_address(reply.substr(offset, end - offset), error);
    if (error) throw std::runtime_error("Invalid DNS helper address.");
    endpoints.emplace_back(address, number);
    offset = end + 1;
  }
  if (endpoints.empty()) throw std::runtime_error("HTTP service discovery resolved no addresses.");
  RemainingMs(deadline);
  return endpoints;
}
}  // namespace ibmw::discovery
