// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/exception.h"

namespace ibmw::runtime::core::runtime_graph {

class RuntimeFileDescriptor {
 public:
  explicit RuntimeFileDescriptor(int fd = -1) : fd_(fd) {}
  ~RuntimeFileDescriptor() { if (fd_ >= 0) ::close(fd_); }
  RuntimeFileDescriptor(const RuntimeFileDescriptor&) = delete;
  RuntimeFileDescriptor& operator=(const RuntimeFileDescriptor&) = delete;
  RuntimeFileDescriptor(RuntimeFileDescriptor&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)) {}
  RuntimeFileDescriptor& operator=(RuntimeFileDescriptor&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0) ::close(fd_);
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  int Get() const noexcept { return fd_; }

 private:
  int fd_;
};

// Walk trusted directories by fd. Only root-owned sticky ancestors such as
// /tmp may be writable by other users; the final directory must be private.
inline RuntimeFileDescriptor OpenRuntimeDirectory(
    const std::filesystem::path& path, bool create) {
  MW_ASSERT(!path.empty() && path.string().find('\0') == std::string::npos,
            "Invalid runtime directory.");
  const auto absolute = std::filesystem::absolute(path);
  RuntimeFileDescriptor directory(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  MW_ASSERT(directory.Get() >= 0, "Cannot open root directory.");
  for (const auto& component : absolute.relative_path()) {
    if (component.empty() || component == ".") continue;
    MW_ASSERT(component != "..", "Runtime directory must not contain '..'.");
    if (create && ::mkdirat(directory.Get(), component.c_str(), 0700) != 0) {
      MW_ASSERT(errno == EEXIST, "Cannot create runtime directory '{}'.", path.string());
    }
    RuntimeFileDescriptor child(::openat(directory.Get(), component.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    MW_ASSERT(child.Get() >= 0, "Cannot safely open runtime directory '{}'.", path.string());
    struct stat status {};
    MW_ASSERT(::fstat(child.Get(), &status) == 0, "Cannot inspect runtime directory.");
    const bool trusted_owner = status.st_uid == ::geteuid() || status.st_uid == 0;
    const bool protected_ancestor = (status.st_mode & 022) == 0 ||
        (status.st_uid == 0 && (status.st_mode & S_ISVTX) != 0);
    MW_ASSERT(trusted_owner && protected_ancestor,
              "Untrusted runtime directory ancestor '{}'.", path.string());
    directory = std::move(child);
  }
  struct stat status {};
  MW_ASSERT(::fstat(directory.Get(), &status) == 0 &&
                status.st_uid == ::geteuid() && (status.st_mode & 077) == 0,
            "Runtime directory '{}' must be owned by the current UID with private permissions.",
            path.string());
  return directory;
}

// Reads the whole file when it holds fewer than max_size bytes. The bound is
// on the bytes read, not on st_size, which may change or be 0 (procfs).
inline std::optional<std::string> ReadRuntimeFileBelow(int fd, size_t max_size) {
  std::string data;
  char buffer[64 * 1024];
  while (true) {
    const ssize_t received = ::read(fd, buffer, sizeof(buffer));
    if (received < 0 && errno == EINTR) continue;
    if (received < 0) return std::nullopt;
    if (received == 0) return data;
    data.append(buffer, static_cast<size_t>(received));
    if (data.size() >= max_size) return std::nullopt;
  }
}

}  // namespace ibmw::runtime::core::runtime_graph
