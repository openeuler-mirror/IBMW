# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Crash signal handling and stack capture; replaces backward-cpp.
add_library(ibmw_selfdev_crash_handler STATIC src/crash_handler.cc)
add_library(ibmw::selfdev::crash_handler ALIAS ibmw_selfdev_crash_handler)

target_include_directories(
  ibmw_selfdev_crash_handler
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_link_libraries(ibmw_selfdev_crash_handler PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
set_target_properties(ibmw_selfdev_crash_handler PROPERTIES POSITION_INDEPENDENT_CODE ON)
ibmw_selfdev_warnings(ibmw_selfdev_crash_handler)
ibmw_selfdev_install(ibmw_selfdev_crash_handler ibmw::selfdev::crash_handler)

if(IBMW_BUILD_TESTS AND NOT WIN32)
  ibmw_selfdev_add_test(ibmw_selfdev_crash_handler_test
    SRCS test/crash_handler_test.cc
    LIBS ibmw::selfdev::crash_handler)
  # Export test symbols so dladdr can name them (CH-03, CH-09, CH-12).
  set_target_properties(ibmw_selfdev_crash_handler_test PROPERTIES ENABLE_EXPORTS ON)
  # Keep crash frames intact, and keep test helpers visible to dladdr even
  # when the parent project builds with -fvisibility=hidden (IBMW does).
  target_compile_options(ibmw_selfdev_crash_handler_test PRIVATE -fno-omit-frame-pointer -fvisibility=default)

  # CH-33: a library whose constructor blocks while dlopen holds the loader
  # lock. Only ever loaded by the test (never linked).
  add_library(ibmw_selfdev_crash_handler_blocking_ctor MODULE test/blocking_ctor.cc)
  ibmw_selfdev_warnings(ibmw_selfdev_crash_handler_blocking_ctor)
  add_dependencies(ibmw_selfdev_crash_handler_test ibmw_selfdev_crash_handler_blocking_ctor)
  target_compile_definitions(ibmw_selfdev_crash_handler_test PRIVATE
    IBMW_CRASH_TEST_BLOCKING_SO="$<TARGET_FILE:ibmw_selfdev_crash_handler_blocking_ctor>")

  # CH-39..CH-41: a library dlopen'ed after Install() (like an IBMW package).
  add_library(ibmw_selfdev_crash_handler_late_lib MODULE test/late_lib.cc)
  ibmw_selfdev_warnings(ibmw_selfdev_crash_handler_late_lib)
  # Not instrumented under sanitizer builds: the overflow and the null store
  # must fault in the library's own code, not in a sanitizer runtime call.
  target_compile_options(ibmw_selfdev_crash_handler_late_lib PRIVATE -fno-omit-frame-pointer -fno-sanitize=all)
  add_dependencies(ibmw_selfdev_crash_handler_test ibmw_selfdev_crash_handler_late_lib)
  target_compile_definitions(ibmw_selfdev_crash_handler_test PRIVATE
    IBMW_CRASH_TEST_LATE_SO="$<TARGET_FILE:ibmw_selfdev_crash_handler_late_lib>")
  target_link_libraries(ibmw_selfdev_crash_handler_test PRIVATE ${CMAKE_DL_LIBS})
endif()
