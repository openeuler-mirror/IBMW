// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include <dlfcn.h>
#if defined(__GLIBC__) || defined(RTLD_DI_LINKMAP)
  #include <link.h>
#endif

namespace ibmw::common::util {

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

#if defined(IBMW_ENABLE_DLOPEN_DEEPBIND) && defined(RTLD_DEEPBIND)
    handle_ = dlopen(lib_name_.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
#else
    handle_ = dlopen(lib_name_.c_str(), RTLD_NOW | RTLD_LOCAL);
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
    // dlerror() returns NULL when no error is pending (or it was already read).
    const char* err = dlerror();
    return err != nullptr ? std::string(err) : std::string("no dynamic library error pending");
  }

 private:
  DynlibHandle handle_ = nullptr;
  std::string lib_name_;
  std::string lib_full_path_;
};

}  // namespace ibmw::common::util
