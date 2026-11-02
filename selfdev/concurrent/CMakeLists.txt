# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Header-only concurrent containers; replace oneTBB.
add_library(ibmw_selfdev_concurrent INTERFACE)
add_library(ibmw::selfdev::concurrent ALIAS ibmw_selfdev_concurrent)

target_include_directories(
  ibmw_selfdev_concurrent
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>)
target_link_libraries(ibmw_selfdev_concurrent INTERFACE Threads::Threads)
target_compile_features(ibmw_selfdev_concurrent INTERFACE cxx_std_17)
# Fences instead of membarrier(2) in BoundedBlockingQueue, for targets whose
# sandbox forbids the call. INTERFACE so that every user compiles the same
# variant (see detail/asymmetric_barrier.h); at run time the environment
# variable IBMW_SELFDEV_NO_MEMBARRIER=1 does the same without a rebuild.
# ON by default for OHOS: a seccomp policy that kills the process on
# membarrier(2) cannot be caught at run time, and on the one board measured
# (HongMeng kernel 1.12, aarch64) the call works but gives no clear gain over
# fences.
# The option is cached: the default only applies to a fresh build directory.
if(OHOS OR CMAKE_SYSTEM_NAME STREQUAL "OHOS" OR "${OHOS_PLATFORM}" STREQUAL "OHOS")
  set(_ibmw_selfdev_no_membarrier_default ON)
else()
  set(_ibmw_selfdev_no_membarrier_default OFF)
endif()
option(IBMW_SELFDEV_CONCURRENT_NO_MEMBARRIER
  "Never use membarrier(2) in selfdev concurrent; use seq_cst fences." ${_ibmw_selfdev_no_membarrier_default})
if(IBMW_SELFDEV_CONCURRENT_NO_MEMBARRIER)
  target_compile_definitions(ibmw_selfdev_concurrent INTERFACE IBMW_SELFDEV_NO_MEMBARRIER)
endif()
ibmw_selfdev_install(ibmw_selfdev_concurrent ibmw::selfdev::concurrent)

if(IBMW_BUILD_TESTS)
  ibmw_selfdev_add_test(ibmw_selfdev_concurrent_test
    SRCS test/bounded_blocking_queue_test.cc
         test/concurrent_hash_map_test.cc
    LIBS ibmw::selfdev::concurrent)
  # Wraps the global operator new (operator new(std::size_t), mangled
  # _Znwm) to fail allocations on demand. Not replaced: Clang links its
  # sanitizer runtimes, which define it too, whole-archive.
  ibmw_selfdev_add_test(ibmw_selfdev_concurrent_alloc_test
    SRCS test/bounded_blocking_queue_alloc_test.cc
    LIBS ibmw::selfdev::concurrent)
  target_link_options(ibmw_selfdev_concurrent_alloc_test PRIVATE "LINKER:--wrap=_Znwm")
  # Membarrier opt-out and failure; run once as is and once opted out.
  ibmw_selfdev_add_test(ibmw_selfdev_concurrent_barrier_test
    SRCS test/bounded_blocking_queue_barrier_test.cc
    LIBS ibmw::selfdev::concurrent)
  add_test(NAME ibmw_selfdev_concurrent_barrier_env_test COMMAND ibmw_selfdev_concurrent_barrier_test)
  set_tests_properties(ibmw_selfdev_concurrent_barrier_env_test
    PROPERTIES ENVIRONMENT IBMW_SELFDEV_NO_MEMBARRIER=1)
endif()
