# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Zero-copy shared memory transport; replaces iceoryx (posh + RouDi) for
# IBMW's iceoryx_ext. Linux only: memfd, futex, epoll, inotify, SCM_RIGHTS.
add_library(ibmw_selfdev_shm STATIC
  src/listener.cc
  src/platform.cc
  src/publisher.cc
  src/runtime.cc
  src/subscriber.cc)
add_library(ibmw::selfdev::shm ALIAS ibmw_selfdev_shm)

target_include_directories(
  ibmw_selfdev_shm
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(ibmw_selfdev_shm PUBLIC Threads::Threads)
target_compile_features(ibmw_selfdev_shm PUBLIC cxx_std_17)
set_target_properties(ibmw_selfdev_shm PROPERTIES POSITION_INDEPENDENT_CODE ON)
ibmw_selfdev_warnings(ibmw_selfdev_shm)
ibmw_selfdev_install(ibmw_selfdev_shm ibmw::selfdev::shm)

if(IBMW_BUILD_TESTS)
  ibmw_selfdev_add_test(ibmw_selfdev_shm_test
    SRCS test/test_util.cc
         test/platform_test.cc
         test/pubsub_test.cc
         test/listener_test.cc
         test/policy_test.cc
         test/robustness_test.cc
    LIBS ibmw::selfdev::shm)
  target_include_directories(ibmw_selfdev_shm_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # Multi-process cases fork/exec this helper.
  add_executable(ibmw_selfdev_shm_peer test/shm_peer.cc)
  target_link_libraries(ibmw_selfdev_shm_peer PRIVATE ibmw::selfdev::shm)
  ibmw_selfdev_warnings(ibmw_selfdev_shm_peer)
  target_compile_definitions(ibmw_selfdev_shm_test PRIVATE
    IBMW_SHM_PEER_PATH="$<TARGET_FILE:ibmw_selfdev_shm_peer>")
  add_dependencies(ibmw_selfdev_shm_test ibmw_selfdev_shm_peer)
  set_tests_properties(ibmw_selfdev_shm_test PROPERTIES TIMEOUT 600)
endif()

option(IBMW_SELFDEV_SHM_BENCH "Build the ibmw_shm latency/CPU benchmark." ON)
if(IBMW_SELFDEV_SHM_BENCH)
  add_executable(ibmw_shm_bench tools/shm_bench.cc)
  target_link_libraries(ibmw_shm_bench PRIVATE ibmw::selfdev::shm)
  ibmw_selfdev_warnings(ibmw_shm_bench)
endif()
