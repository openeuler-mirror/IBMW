// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

// Symbols of the libibmw_selfdev_coro shared library. The library holds the
// one thread_local slot that Task's synchronous-completion handoff relies on
// (see task.h); it must stay a single shared object so that every DSO of a
// process (including modules loaded with RTLD_DEEPBIND, or linked with
// -Bsymbolic / version scripts) uses the same slot.
#if defined(_WIN32)
  #if defined(IBMW_SELFDEV_CORO_BUILDING)
    #define IBMW_SELFDEV_CORO_API __declspec(dllexport)
  #else
    #define IBMW_SELFDEV_CORO_API __declspec(dllimport)
  #endif
#else
  #define IBMW_SELFDEV_CORO_API __attribute__((visibility("default")))
#endif

// For await_suspend bodies that keep running after publishing the coroutine
// handle: keeps their locals on their own stack frame. Clang before 19 may
// place the locals of an await_suspend inlined into the coroutine in the
// coroutine frame, which another thread may already have destroyed.
#if defined(_MSC_VER) && !defined(__clang__)
  #define IBMW_SELFDEV_CORO_NOINLINE __declspec(noinline)
#else
  #define IBMW_SELFDEV_CORO_NOINLINE __attribute__((noinline))
#endif

// Calls from other DSOs into Start()/Finish() (two per co_await): GCC's noplt
// calls through the GOT directly instead of through a PLT stub.
#if defined(__has_attribute)
  #if __has_attribute(noplt)
    #define IBMW_SELFDEV_CORO_NOPLT __attribute__((noplt))
  #endif
#endif
#if !defined(IBMW_SELFDEV_CORO_NOPLT)
  #define IBMW_SELFDEV_CORO_NOPLT
#endif
