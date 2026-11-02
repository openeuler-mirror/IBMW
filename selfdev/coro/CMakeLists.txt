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
target_compile_definitions(ibmw_selfdev_coro PRIVATE IBMW_SELFDEV_CORO_BUILDING)
# SONAME libibmw_selfdev_coro.so.1. Bump SOVERSION on any change to the
# exported symbols or to the layout of the types they take (TaskPromiseBase,
# StartRecord, AsyncScope): packages built against the old headers would
# otherwise load the new library.
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

# initial-exec TLS for the slot: a plain %fs-relative access instead of a
# __tls_get_addr call in Start()/Finish() (one each per co_await). glibc
# reserves static TLS for a few dlopen'ed libraries, which covers this
# 8-byte slot even when the library is first loaded through dlopen(). musl
# has no such reserve: only startup modules get static TLS, and dlopen() of
# a library with initial-exec TLS fails ("initial-exec TLS resolves to
# dynamic definition"). bin/ibmw has a DT_NEEDED on this library
# (src/core/engine/engine.cc references it), so in processes started by
# bin/ibmw it is a startup module and initial-exec is safe on musl too (the
# slot is only accessed from src/task.cc). OHOS stays OFF by default because
# an application process may dlopen IBMW, where ON would make dlopen() fail;
# turn it ON there when the host is known to load the library at startup
# (bin/ibmw deployments).
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
    else()
      message(STATUS "ibmw_selfdev_coro: no nm found, export check ibmw_selfdev_coro_exports skipped")
    endif()
  endif()

  # CO-17: one slot per thread across DSOs (finding: per-DSO slot copies).
  if(UNIX AND NOT APPLE)
    add_subdirectory(test/dso)
  endif()
endif()
