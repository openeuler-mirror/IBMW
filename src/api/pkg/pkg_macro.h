// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "cpp/module_base.h"
#include "cpp/util/string.h"
#include "pkg/pkg_main.h"

// ---------------------------------------------------------------------------
// IBMW_PKG_MAIN — generates the single package entry point IbmwPkgEntryV2().
//
// Usage:
//   static std::tuple<std::string_view, std::function<ibmw::Module*()>>
//       ibmw_module_register_array[]{
//           {"ModuleName", []() -> ibmw::Module* { return new MyModule(); }},
//       };
//   IBMW_PKG_MAIN(ibmw_module_register_array)
// ---------------------------------------------------------------------------
#define IBMW_PKG_MAIN(__ibmw_module_register_array__)                                           \
  static constexpr size_t __ibmw_module_register_array_size__ =                                 \
      sizeof(__ibmw_module_register_array__) / sizeof(__ibmw_module_register_array__[0]);       \
                                                                                                 \
  /* Static cache of created module instances, keyed by module name. Used by */                \
  /* __IbmwPkgSetDriverSlot to dispatch driver pointers without re-running   */                \
  /* the factory. Lives for the lifetime of the dynamic library.             */                \
  static std::unordered_map<std::string, ibmw::Module*>& __ibmw_pkg_module_instances__() {       \
    static std::unordered_map<std::string, ibmw::Module*> instances;                             \
    return instances;                                                                            \
  }                                                                                              \
                                                                                                 \
  /* Guards the instance cache: the host may create/destroy modules from several threads. */     \
  static std::mutex& __ibmw_pkg_module_instances_mutex__() {                                     \
    static std::mutex mtx;                                                                       \
    return mtx;                                                                                  \
  }                                                                                              \
                                                                                                 \
  static size_t __IbmwPkgGetModuleNum() {                                                       \
    return __ibmw_module_register_array_size__;                                                 \
  }                                                                                             \
                                                                                                 \
  static const ibmw_string_view_t* __IbmwPkgGetModuleNameList() {                               \
    static const ibmw_string_view_t* module_name_list = []() {                                  \
      static ibmw_string_view_t module_name_array[__ibmw_module_register_array_size__];         \
      for (size_t idx = 0; idx < __ibmw_module_register_array_size__; ++idx) {                  \
        module_name_array[idx] = ibmw::util::ToIbmwStringView(                                  \
            std::get<0>(__ibmw_module_register_array__[idx]));                                  \
      }                                                                                         \
      return module_name_array;                                                                 \
    }();                                                                                        \
    return module_name_list;                                                                    \
  }                                                                                             \
                                                                                                 \
  static const ibmw_module_base_t* __IbmwPkgCreateModule(ibmw_string_view_t module_name) {       \
    std::string_view module_name_str = ibmw::util::ToStdStringView(module_name);                 \
    for (size_t idx = 0; idx < __ibmw_module_register_array_size__; ++idx) {                     \
      std::string_view cur_module_name_str = std::get<0>(__ibmw_module_register_array__[idx]);   \
      if (module_name_str == cur_module_name_str) {                                              \
        /* extern "C" boundary: a throwing factory must not unwind into the host. */             \
        ibmw::Module* module_ptr = nullptr;                                                      \
        try {                                                                                    \
          module_ptr = (std::get<1>(__ibmw_module_register_array__[idx]))();                     \
        } catch (...) {                                                                          \
          return nullptr;                                                                        \
        }                                                                                        \
        if (module_ptr == nullptr) return nullptr;                                               \
        try {                                                                                    \
          std::lock_guard<std::mutex> lck(__ibmw_pkg_module_instances_mutex__());                \
          __ibmw_pkg_module_instances__()[std::string(module_name_str)] = module_ptr;            \
        } catch (...) {                                                                          \
          delete module_ptr;                                                                     \
          return nullptr;                                                                        \
        }                                                                                        \
        return module_ptr->NativeHandle();                                                       \
      }                                                                                          \
    }                                                                                            \
    return nullptr;                                                                              \
  }                                                                                              \
                                                                                                 \
  static void __IbmwPkgDestroyModule(const ibmw_module_base_t* module_ptr) {                     \
    if (module_ptr == nullptr) return;                                                           \
    auto* mod = static_cast<ibmw::Module*>(module_ptr->ctx);                                     \
    {                                                                                            \
      /* Drop from instance cache before delete; SetDriverSlot post-shutdown becomes safe-no-op. */\
      std::lock_guard<std::mutex> lck(__ibmw_pkg_module_instances_mutex__());                    \
      for (auto it = __ibmw_pkg_module_instances__().begin();                                    \
           it != __ibmw_pkg_module_instances__().end(); ++it) {                                  \
        if (it->second == mod) {                                                                 \
          __ibmw_pkg_module_instances__().erase(it);                                             \
          break;                                                                                 \
        }                                                                                        \
      }                                                                                          \
    }                                                                                            \
    delete mod;                                                                                  \
  }                                                                                              \
                                                                                                 \
  /* Typed-slot dispatch: forwards each (slot_id, driver_ptr) pair to the    */                \
  /* matching module instance's SetDriverSlot override. Modules opt in via   */                \
  /* IBMW_USE_DRIVER / IBMW_USE_DRIVERS in cpp/module_base.h.                */                \
  static void __IbmwPkgSetDriverSlot(ibmw_string_view_t module_name,                            \
                                     ibmw_driver_slot_t slot_id,                                \
                                     void* driver_ptr) {                                        \
    std::string_view module_name_str = ibmw::util::ToStdStringView(module_name);                \
    for (size_t idx = 0; idx < __ibmw_module_register_array_size__; ++idx) {                    \
      std::string_view cur_module_name_str = std::get<0>(__ibmw_module_register_array__[idx]);  \
      if (module_name_str == cur_module_name_str) {                                             \
        /* Look up under the lock, call outside it: the user override may     */                \
        /* create/destroy modules. The host serializes this with destroy.     */                \
        try {                                                                                   \
          ibmw::Module* module_ptr = nullptr;                                                   \
          {                                                                                     \
            std::lock_guard<std::mutex> lck(__ibmw_pkg_module_instances_mutex__());              \
            auto cache_itr = __ibmw_pkg_module_instances__().find(std::string(module_name_str)); \
            if (cache_itr != __ibmw_pkg_module_instances__().end()) module_ptr = cache_itr->second; \
          }                                                                                     \
          if (module_ptr) module_ptr->SetDriverSlot(slot_id, driver_ptr);                       \
        } catch (...) {                                                                         \
        }                                                                                       \
        return;                                                                                 \
      }                                                                                         \
    }                                                                                           \
  }                                                                                             \
                                                                                                 \
  extern "C" {                                                                                  \
  IBMW_PKG_EXPORT ibmw_pkg_entry_v2_t IbmwPkgEntryV2() {                                        \
    return ibmw_pkg_entry_v2_t{                                                                 \
        .abi_version = IBMW_PKG_ABI_VERSION_V2,                                                 \
        .struct_size = sizeof(ibmw_pkg_entry_v2_t),                                             \
        .get_module_num = __IbmwPkgGetModuleNum,                                                \
        .get_module_name_list = __IbmwPkgGetModuleNameList,                                     \
        .create_module = __IbmwPkgCreateModule,                                                 \
        .destroy_module = __IbmwPkgDestroyModule,                                               \
        .set_driver_slot = __IbmwPkgSetDriverSlot};                                             \
  }                                                                                             \
  }
