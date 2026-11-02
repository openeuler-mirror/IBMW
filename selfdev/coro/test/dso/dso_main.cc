// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// CO-17 driver:  dso_main <module.so> <deepbind|local> <iterations>
// Loads the module with dlopen and checks that the module, the host library
// and (when linked) this executable all see the same per-thread slot, then
// runs the module's synchronous co_await loop. Exit code 0 on success.
//
// Built twice: linked against ibmw::selfdev::coro and the host library
// (CORO_DSO_LINKED), and linked against nothing but libdl, so that the coro
// library is first loaded by dlopen() (initial-exec TLS from a dlopen'ed
// library).

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(CORO_DSO_LINKED)
  #include "ibmw_selfdev/coro/task.h"
extern "C" void* CoroDsoHostSlot();
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
  #define CORO_DSO_SANITIZER 1
#elif defined(__has_feature)
  #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    #define CORO_DSO_SANITIZER 1
  #endif
#endif

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <module.so> <deepbind|local> <iterations>\n", argv[0]);
    return 2;
  }
  int flags = RTLD_NOW | RTLD_LOCAL;
  if (std::strcmp(argv[2], "deepbind") == 0) {
#if defined(CORO_DSO_SANITIZER)
    // The sanitizer runtimes refuse RTLD_DEEPBIND (they abort in dlopen);
    // sanitizer lanes run the same module with RTLD_LOCAL only.
    std::printf("sanitizer build: RTLD_DEEPBIND not supported, using RTLD_LOCAL\n");
#else
    flags |= RTLD_DEEPBIND;
#endif
  }
  const int n = std::atoi(argv[3]);

  void* module = dlopen(argv[1], flags);
  if (module == nullptr) {
    std::fprintf(stderr, "dlopen: %s\n", dlerror());
    return 2;
  }
  auto module_slot = reinterpret_cast<void* (*)()>(dlsym(module, "CoroDsoModuleSlot"));
  auto module_loop = reinterpret_cast<long (*)(int)>(dlsym(module, "CoroDsoModuleLoop"));
  auto host_slot = reinterpret_cast<void* (*)()>(dlsym(module, "CoroDsoHostSlot"));
  if (module_slot == nullptr || module_loop == nullptr || host_slot == nullptr) {
    std::fprintf(stderr, "dlsym failed\n");
    return 2;
  }

  int failures = 0;
  void* const slot = module_slot();
  std::printf("module slot %p, host slot %p\n", slot, host_slot());
  if (slot != host_slot()) ++failures;
#if defined(CORO_DSO_LINKED)
  void* const own = &ibmw::selfdev::coro::detail::CurrentStartRecord();
  std::printf("executable slot %p, host slot (direct) %p\n", own, CoroDsoHostSlot());
  if (slot != own || own != CoroDsoHostSlot()) ++failures;
#endif

  const long sum = module_loop(n);
  std::printf("sum=%ld expect=%d\n", sum, n);
  if (sum != n) ++failures;

  std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
