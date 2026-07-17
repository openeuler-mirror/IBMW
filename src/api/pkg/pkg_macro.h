// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "cpp/module_base.h"
#include "cpp/util/string.h"
#include "pkg/pkg_main.h"

// ---------------------------------------------------------------------------
// IBMW_PKG_MAIN — generates the single package entry point IbmwPkgEntry().
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
  static std::unordered_map<std::string, ibmw::Module*>& __ibmw_pkg_module_instances__() {     \
    static std::unordered_map<std::string, ibmw::Module*> instances;                            \
    return instances;                                                                            \
  }                                                                                             \
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
  static const ibmw_module_base_t* __IbmwPkgCreateModule(ibmw_string_view_t module_name) {      \
    std::string_view module_name_str = ibmw::util::ToStdStringView(module_name);                \
    for (size_t idx = 0; idx < __ibmw_module_register_array_size__; ++idx) {                    \
      std::string_view cur_module_name_str = std::get<0>(__ibmw_module_register_array__[idx]);  \
      if (module_name_str == cur_module_name_str) {                                             \
        ibmw::Module* module_ptr = (std::get<1>(__ibmw_module_register_array__[idx]))();        \
        __ibmw_pkg_module_instances__()[std::string(module_name_str)] = module_ptr;             \
        return module_ptr->NativeHandle();                                                      \
      }                                                                                         \
    }                                                                                           \
    return nullptr;                                                                             \
  }                                                                                             \
                                                                                                 \
  static void __IbmwPkgDestroyModule(const ibmw_module_base_t* module_ptr) {                    \
    auto* mod = static_cast<ibmw::Module*>(module_ptr->ctx);                                    \
    /* Drop from instance cache before delete; SetDriverSlot post-shutdown becomes safe-no-op. */ \
    for (auto it = __ibmw_pkg_module_instances__().begin();                                     \
         it != __ibmw_pkg_module_instances__().end(); ++it) {                                   \
      if (it->second == mod) {                                                                  \
        __ibmw_pkg_module_instances__().erase(it);                                              \
        break;                                                                                   \
      }                                                                                         \
    }                                                                                           \
    delete mod;                                                                                 \
  }                                                                                             \
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
        auto cache_itr = __ibmw_pkg_module_instances__().find(std::string(module_name_str));   \
        if (cache_itr != __ibmw_pkg_module_instances__().end() && cache_itr->second) {        \
          cache_itr->second->SetDriverSlot(slot_id, driver_ptr);                                \
        }                                                                                       \
        return;                                                                                 \
      }                                                                                         \
    }                                                                                           \
  }                                                                                             \
                                                                                                 \
  extern "C" {                                                                                  \
  IBMW_PKG_EXPORT ibmw_pkg_entry_t IbmwPkgEntry() {                                            \
    return ibmw_pkg_entry_t{                                                                    \
        .get_module_num = __IbmwPkgGetModuleNum,                                                \
        .get_module_name_list = __IbmwPkgGetModuleNameList,                                     \
        .create_module = __IbmwPkgCreateModule,                                                 \
        .destroy_module = __IbmwPkgDestroyModule,                                               \
        .set_driver_slot = __IbmwPkgSetDriverSlot};                                             \
  }                                                                                             \
  }
