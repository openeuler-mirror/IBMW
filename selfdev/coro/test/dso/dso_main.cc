// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// CO-17 driver:  dso_main <module.so> <deepbind|local> <iterations> [pre-thread]
// Loads the module with dlopen and checks that the module, the host library
// and (when linked) this executable all see the same per-thread slot, then
// runs the module's synchronous co_await loop. Exit code 0 on success.
// With `pre-thread` the checks and the loop run on a thread started before
// dlopen() (an application that dlopens IBMW already has threads; with
// native TLS the loader must give them the new library's slot); that
// thread's slot must differ from the main thread's, and the main thread's
// slot must still match the host's.
//
// Built twice: linked against ibmw::selfdev::coro and the host library
// (CORO_DSO_LINKED), and linked against nothing but libdl, so that the coro
// library is first loaded by dlopen() (initial-exec TLS from a dlopen'ed
// library).

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>

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

namespace {

struct Module {
  void* (*slot)();
  long (*loop)(int);
  void* (*host_slot)();
};

// The slot checks and the loop on the calling thread; returns the number of
// failures.
int Check(const Module& m, int n) {
  int failures = 0;
  void* const slot = m.slot();
  std::printf("module slot %p, host slot %p\n", slot, m.host_slot());
  if (slot != m.host_slot()) ++failures;
#if defined(CORO_DSO_LINKED)
  void* const own = &ibmw::selfdev::coro::detail::CurrentStartRecord();
  std::printf("executable slot %p, host slot (direct) %p\n", own, CoroDsoHostSlot());
  if (slot != own || own != CoroDsoHostSlot()) ++failures;
#endif

  const long sum = m.loop(n);
  std::printf("sum=%ld expect=%d\n", sum, n);
  if (sum != n) ++failures;
  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  const bool pre_thread = argc == 5 && std::strcmp(argv[4], "pre-thread") == 0;
  if (argc != 4 && !pre_thread) {
    std::fprintf(stderr, "usage: %s <module.so> <deepbind|local> <iterations> [pre-thread]\n", argv[0]);
    return 2;
  }
  int flags = RTLD_NOW | RTLD_LOCAL;
  if (std::strcmp(argv[2], "deepbind") == 0) {
#if defined(CORO_DSO_SANITIZER)
    // The sanitizer runtimes refuse RTLD_DEEPBIND (they abort in dlopen);
    // sanitizer lanes run the same module with RTLD_LOCAL only.
    std::printf("sanitizer build: RTLD_DEEPBIND not supported, using RTLD_LOCAL\n");
#elif defined(RTLD_DEEPBIND)
    flags |= RTLD_DEEPBIND;
#else
    // musl (OHOS) does not have RTLD_DEEPBIND.
    std::printf("RTLD_DEEPBIND not available, using RTLD_LOCAL\n");
#endif
  }
  const int n = std::atoi(argv[3]);

  // Started before dlopen(); waits for the loaded module.
  std::promise<Module> loaded;
  int pre_failures = 0;
  void* pre_slot = nullptr;
  std::thread pre;
  if (pre_thread) {
    pre = std::thread([&loaded, &pre_failures, &pre_slot, n] {
      const Module m = loaded.get_future().get();
      if (m.slot == nullptr) return;
      pre_slot = m.slot();
      std::printf("pre-thread: ");
      pre_failures = Check(m, n);
    });
  }

  void* module = dlopen(argv[1], flags);
  Module m{};
  if (module != nullptr) {
    m.slot = reinterpret_cast<void* (*)()>(dlsym(module, "CoroDsoModuleSlot"));
    m.loop = reinterpret_cast<long (*)(int)>(dlsym(module, "CoroDsoModuleLoop"));
    m.host_slot = reinterpret_cast<void* (*)()>(dlsym(module, "CoroDsoHostSlot"));
  }
  const bool ok = m.slot != nullptr && m.loop != nullptr && m.host_slot != nullptr;
  if (pre_thread) {
    loaded.set_value(ok ? m : Module{});
    pre.join();
  }
  if (module == nullptr) {
    std::fprintf(stderr, "dlopen: %s\n", dlerror());
    return 2;
  }
  if (!ok) {
    std::fprintf(stderr, "dlsym failed\n");
    return 2;
  }

  int failures = 0;
  if (pre_thread) {
    // Each thread has its own slot, and all DSOs agree on the main thread's.
    void* const main_slot = m.slot();
    std::printf("main thread slot %p, host slot %p\n", main_slot, m.host_slot());
    failures += pre_failures;
    if (main_slot == pre_slot || main_slot != m.host_slot()) ++failures;
  } else {
    failures += Check(m, n);
  }

  std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
