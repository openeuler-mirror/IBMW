# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# C++20 coroutine primitives; replace libunifex / stdexec behind ibmw::co::*.
# Mostly header-only. src/task.cc holds the per-thread slot of Task's
# synchronous-completion handoff (src/start_detached.cc the process-wide
# StartDetached scope); the library is always built as SHARED:
# every DSO in the process must use the same slot (RTLD_DEEPBIND, -Bsymbolic
# and version scripts would otherwise give each DSO its own copy; see
# include/ibmw_selfdev/coro/task.h). Never build it as a static library.
add_library(ibmw_selfdev_coro SHARED src/task.cc src/start_detached.cc)
add_library(ibmw::selfdev::coro ALIAS ibmw_selfdev_coro)

target_include_directories(
  ibmw_selfdev_coro
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(ibmw_selfdev_coro PUBLIC Threads::Threads)
target_compile_features(ibmw_selfdev_coro PUBLIC cxx_std_20)
# SONAME libibmw_selfdev_coro.so.1. Bump SOVERSION on any change to the
# exported symbols or to the layout of the types they take (TaskPromiseBase,
# StartRecord, AsyncScope): packages built against the old headers would
# otherwise load the new library. A toolchain that sets
# CMAKE_PLATFORM_NO_VERSIONED_SONAME (cmake/toolchains/ohos-arm64-v8a.cmake)
# ignores SOVERSION: the SONAME is the plain libibmw_selfdev_coro.so there.
set_target_properties(ibmw_selfdev_coro PROPERTIES
  VERSION 1.0.0
  SOVERSION 1
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)
# Export only the ibmw::selfdev::coro symbols marked IBMW_SELFDEV_CORO_API.
# Hidden visibility alone does not hide what libstdc++ declares with default
# visibility (std::vector<coroutine_handle<>> members, the STB_GNU_UNIQUE
# noop_coroutine frame, exception_ptr inlines in Debug builds, gcov symbols
# with --coverage). Checked by ibmw_selfdev_coro_exports below.
if(UNIX AND NOT APPLE)
  set(_ibmw_selfdev_coro_map ${CMAKE_CURRENT_SOURCE_DIR}/src/libibmw_selfdev_coro.map)
  target_link_options(ibmw_selfdev_coro PRIVATE -Wl,--version-script=${_ibmw_selfdev_coro_map})
  set_property(TARGET ibmw_selfdev_coro APPEND PROPERTY LINK_DEPENDS ${_ibmw_selfdev_coro_map})
endif()

# initial-exec TLS for the slot: a plain thread-pointer-relative access
# instead of a __tls_get_addr call in Start()/Finish() (three per co_await). glibc
# reserves static TLS for a few dlopen'ed libraries, which covers this
# 8-byte slot even when the library is first loaded through dlopen(). musl
# has no such reserve: only startup modules get static TLS, and dlopen() of
# a library with initial-exec TLS fails ("initial-exec TLS resolves to
# dynamic definition"). bin/ibmw has a DT_NEEDED on this library
# (src/core/engine/engine.cc references it), so in processes started by
# bin/ibmw it is a startup module and initial-exec is safe on musl too (the
# slot is only accessed from src/task.cc). OHOS stays OFF by default because
# an application process may dlopen IBMW, where ON (with native TLS, below)
# would make dlopen() fail; turn it ON there when the host is known to load
# the library at startup (bin/ibmw deployments).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT OHOS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(_ibmw_selfdev_coro_tls_ie_default ON)
else()
  set(_ibmw_selfdev_coro_tls_ie_default OFF)
endif()
option(IBMW_SELFDEV_CORO_TLS_INITIAL_EXEC
  "Use the initial-exec TLS model for the selfdev coro thread_local slot." ${_ibmw_selfdev_coro_tls_ie_default})
if(IBMW_SELFDEV_CORO_TLS_INITIAL_EXEC)
  target_compile_definitions(ibmw_selfdev_coro PRIVATE IBMW_SELFDEV_CORO_TLS_INITIAL_EXEC)
endif()

# Native TLS for the slot where the compiler emulates TLS by default (the
# OHOS SDK clang): emulated, each of the three accesses per co_await is a
# __emutls_get_address call; native saves about 10 ns per co_await on the
# board (HongMeng kernel 1.12, aarch64). The slot is private to src/task.cc (other
# DSOs reach it through CurrentStartRecord()), so the library can use native
# TLS whatever its users are built with. The model stays global-dynamic
# (TLSDESC on aarch64), which works both when the library is a startup
# module and when it is first loaded by dlopen(), also for threads started
# before the dlopen() (CO-17 on the board).
# Initial-exec only takes effect with native TLS (emulated TLS ignores the
# model), and then dlopen() fails on musl, see above. On the board the
# chained co_await, SyncWait and AsyncScope spawn microbenchmarks are 4-8%
# faster; pool round trips and pool spawns are unchanged within noise.
# ON by default for OHOS arm64-v8a, the only architecture checked on a board.
# The option is cached: an existing cache entry wins over the default.
# Compile-time only: with LTO the link would need the flag too
# (ibmw_selfdev_coro_native_tls checks the result).
set(_ibmw_selfdev_coro_ohos OFF)
if(OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS" OR "${OHOS_PLATFORM}" STREQUAL "OHOS")
  set(_ibmw_selfdev_coro_ohos ON)
endif()
if(_ibmw_selfdev_coro_ohos AND OHOS_ARCH STREQUAL "arm64-v8a")
  set(_ibmw_selfdev_coro_native_tls_default ON)
else()
  set(_ibmw_selfdev_coro_native_tls_default OFF)
endif()
option(IBMW_SELFDEV_CORO_NATIVE_TLS
  "Build the selfdev coro library with -fno-emulated-tls (native TLS where the compiler emulates it by default)."
  ${_ibmw_selfdev_coro_native_tls_default})
set(_ibmw_selfdev_coro_native_tls OFF)
if(IBMW_SELFDEV_CORO_NATIVE_TLS)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-fno-emulated-tls IBMW_SELFDEV_CORO_HAS_NO_EMULATED_TLS)
  if(IBMW_SELFDEV_CORO_HAS_NO_EMULATED_TLS)
    target_compile_options(ibmw_selfdev_coro PRIVATE -fno-emulated-tls)
    set(_ibmw_selfdev_coro_native_tls ON)
  elseif(_ibmw_selfdev_coro_ohos)
    message(WARNING "ibmw_selfdev_coro: ${CMAKE_CXX_COMPILER} rejects -fno-emulated-tls; the slot stays emulated (slower co_await)")
  else()
    message(STATUS "ibmw_selfdev_coro: ${CMAKE_CXX_COMPILER} has no -fno-emulated-tls; keeping its default TLS")
  endif()
endif()
if(_ibmw_selfdev_coro_ohos AND IBMW_SELFDEV_CORO_TLS_INITIAL_EXEC)
  if(_ibmw_selfdev_coro_native_tls)
    message(WARNING "ibmw_selfdev_coro: initial-exec with native TLS; dlopen() of the library fails on musl, "
                    "use only when every process loads it at startup (bin/ibmw)")
  else()
    message(STATUS "ibmw_selfdev_coro: IBMW_SELFDEV_CORO_TLS_INITIAL_EXEC has no effect with emulated TLS")
  endif()
endif()
ibmw_selfdev_warnings(ibmw_selfdev_coro)
ibmw_selfdev_install(ibmw_selfdev_coro ibmw::selfdev::coro)

if(IBMW_BUILD_TESTS)
  ibmw_selfdev_add_test(ibmw_selfdev_coro_test
    SRCS test/task_test.cc
         test/scheduler_test.cc
         test/async_scope_test.cc
         test/sync_wait_test.cc
         test/awaiter_rules_test.cc
    LIBS ibmw::selfdev::coro)
  target_include_directories(ibmw_selfdev_coro_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)

  # CO-26: the dynamic symbol table of the library is exactly the allow-list.
  if(UNIX AND NOT APPLE)
    set(_ibmw_selfdev_coro_nm "${CMAKE_NM}")
    if(NOT _ibmw_selfdev_coro_nm)
      find_program(IBMW_SELFDEV_CORO_NM NAMES llvm-nm nm)
      set(_ibmw_selfdev_coro_nm "${IBMW_SELFDEV_CORO_NM}")
    endif()
    if(_ibmw_selfdev_coro_nm)
      add_test(NAME ibmw_selfdev_coro_exports
               COMMAND ${CMAKE_COMMAND} -DNM=${_ibmw_selfdev_coro_nm} -DLIB=$<TARGET_FILE:ibmw_selfdev_coro>
                       -DALLOW=${CMAKE_CURRENT_SOURCE_DIR}/test/exports.txt
                       -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_exports.cmake)
      if(_ibmw_selfdev_coro_native_tls)
        # IBMW_SELFDEV_CORO_NATIVE_TLS took effect: no __emutls_get_address import.
        add_test(NAME ibmw_selfdev_coro_native_tls
                 COMMAND ${CMAKE_COMMAND} -DNM=${_ibmw_selfdev_coro_nm} -DLIB=$<TARGET_FILE:ibmw_selfdev_coro>
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/test/check_native_tls.cmake)
      endif()
    else()
      message(STATUS "ibmw_selfdev_coro: no nm found, export check ibmw_selfdev_coro_exports skipped")
    endif()
  endif()

  # CO-17: one slot per thread across DSOs (finding: per-DSO slot copies).
  if(UNIX AND NOT APPLE)
    add_subdirectory(test/dso)
  endif()
endif()
