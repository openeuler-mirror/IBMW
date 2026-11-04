// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(RTLD_DI_LINKMAP)
  #include <link.h>
#endif

namespace ibmw::common::util {

// CORE-017: a library given by path (anything with a '/') must not be
// replaceable by other users. The file and its directory (after resolving
// symlinks) must be owned by the effective user or root and not writable by
// others; a sticky directory such as /tmp may be. Group write is allowed.
// Ancestor directories are not checked. Bare names keep the loader search path.
// Returns the refusal reason, or an empty string and sets dlopen_name to the
// checked (resolved) path, so a symlink re-pointed after the check is not
// followed. One canonical() and two stat() calls per load.
inline std::string CheckLibraryTrust(const std::string& lib_name, std::string& dlopen_name) {
  dlopen_name = lib_name;
  if (lib_name.find('/') == std::string::npos) return {};
  std::error_code ec;
  const auto file = std::filesystem::canonical(lib_name, ec);
  struct stat status {};
  if (ec || ::stat(file.c_str(), &status) != 0) return {};  // dlopen reports it
  const auto trusted_owner = [](const struct stat& st) { return st.st_uid == ::geteuid() || st.st_uid == 0; };
  if (!trusted_owner(status) || (status.st_mode & S_IWOTH) != 0) {
    return "Refusing to load '" + file.string() +
           "': the library must be owned by the current user or root and not writable by others.";
  }
  const auto directory = file.parent_path();
  if (::stat(directory.c_str(), &status) != 0 || !trusted_owner(status) ||
      ((status.st_mode & S_IWOTH) != 0 && (status.st_mode & S_ISVTX) == 0)) {
    return "Refusing to load '" + file.string() + "': directory '" + directory.string() +
           "' must be owned by the current user or root and not writable by others.";
  }
  dlopen_name = file.string();
  return {};
}

class DynamicLib {
 public:
  typedef void* SymbolType;
  typedef void* DynlibHandle;

 public:
  DynamicLib() = default;
  ~DynamicLib() { Unload(); }

  DynamicLib(const DynamicLib&) = delete;
  DynamicLib& operator=(const DynamicLib&) = delete;

  void Swap(DynamicLib& other) noexcept {
    using std::swap;
    swap(handle_, other.handle_);
    swap(lib_name_, other.lib_name_);
    swap(lib_full_path_, other.lib_full_path_);
  }

  bool Load(const std::string& lib_name) {
    Unload();
    lib_name_ = lib_name;
    std::string dlopen_name;
    LoadError() = CheckLibraryTrust(lib_name_, dlopen_name);
    if (!LoadError().empty()) {
      (void)dlerror();  // GetErr() must not report an older error next time
      return false;
    }

#if defined(IBMW_ENABLE_DLOPEN_DEEPBIND) && defined(RTLD_DEEPBIND)
    handle_ = dlopen(dlopen_name.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
#else
    handle_ = dlopen(dlopen_name.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif

    if (nullptr == handle_) return false;

#if defined(__GLIBC__) || defined(RTLD_DI_LINKMAP)
    // RTLD_DI_* are enumerators on glibc, so test for the libc, not the macro.
    // l_name is the path the loader actually opened (after search-path
    // resolution). Never throw here: dlopen has already succeeded.
    lib_full_path_ = lib_name_;
    struct link_map* lm = nullptr;
    if (dlinfo(handle_, RTLD_DI_LINKMAP, &lm) == 0 && lm != nullptr && lm->l_name != nullptr &&
        lm->l_name[0] != '\0') {
      std::error_code ec;
      auto full_path = std::filesystem::canonical(std::filesystem::path(lm->l_name), ec);
      lib_full_path_ = ec ? std::string(lm->l_name) : full_path.string();
    }
#else
    try {
      lib_full_path_ = std::filesystem::absolute(lib_name_).string();
    } catch (...) {
      lib_full_path_ = lib_name_;
    }
#endif

    return true;
  }

  void Unload() {
    if (nullptr == handle_) return;

    bool ret = false;
    ret = (0 == dlclose(handle_));

    if (!ret) {
      return;
    }

    handle_ = nullptr;
  }

  bool IsLoaded() const { return nullptr != handle_; }

  // Relinquish ownership of the handle without calling dlclose.
  // Used to intentionally leak .so files whose static destructors would
  // crash if invoked after dependent resources are already freed.
  void LeakHandle() { handle_ = nullptr; }

  SymbolType GetSymbol(const std::string& symbol_name) {
    if (nullptr == handle_)
      throw std::runtime_error("DynamicLib does not load any lib.");

    return dlsym(handle_, symbol_name.c_str());
  }

  const std::string& GetLibName() const { return lib_name_; }

  const std::string& GetLibFullPath() const { return lib_full_path_; }

  static std::string GetErr() {
    if (!LoadError().empty()) return std::exchange(LoadError(), std::string());
    // dlerror() returns NULL when no error is pending (or it was already read).
    const char* err = dlerror();
    return err != nullptr ? std::string(err) : std::string("no dynamic library error pending");
  }

 private:
  static std::string& LoadError() {
    thread_local std::string error;
    return error;
  }

  DynlibHandle handle_ = nullptr;
  std::string lib_name_;
  std::string lib_full_path_;
};

}  // namespace ibmw::common::util
