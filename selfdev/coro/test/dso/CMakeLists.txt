# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# CO-17: exactly one StartRecord slot per thread across DSOs. A module awaits
# a Task defined in a host library 1M times in a synchronous loop, loaded
# with RTLD_DEEPBIND or linked with -Bsymbolic / a version script; each
# module is built at -O0 and -O2 whatever the build type. The pre_thread
# cases run the loop on a thread started before dlopen().

add_library(ibmw_coro_dso_host SHARED dso_host.cc)
target_link_libraries(ibmw_coro_dso_host PRIVATE ibmw::selfdev::coro)
target_link_options(ibmw_coro_dso_host PRIVATE -Wl,-Bsymbolic)
ibmw_selfdev_warnings(ibmw_coro_dso_host)

add_executable(ibmw_coro_dso_main_linked dso_main.cc)
target_compile_definitions(ibmw_coro_dso_main_linked PRIVATE CORO_DSO_LINKED)
target_link_libraries(ibmw_coro_dso_main_linked PRIVATE ibmw::selfdev::coro ibmw_coro_dso_host ${CMAKE_DL_LIBS} Threads::Threads)
ibmw_selfdev_warnings(ibmw_coro_dso_main_linked)

# Links neither the coro library nor the host: both are first loaded by
# dlopen() as dependencies of the module.
add_executable(ibmw_coro_dso_main_dlopen dso_main.cc)
target_link_libraries(ibmw_coro_dso_main_dlopen PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
ibmw_selfdev_warnings(ibmw_coro_dso_main_dlopen)

set(_coro_dso_iterations 1000000)
foreach(opt O0 O2)
  foreach(variant plain bsymbolic vscript)
    set(mod ibmw_coro_dso_module_${variant}_${opt})
    add_library(${mod} MODULE dso_module.cc)
    target_link_libraries(${mod} PRIVATE ibmw::selfdev::coro ibmw_coro_dso_host)
    # Appended after the build-type flags, so it wins.
    target_compile_options(${mod} PRIVATE -${opt})
    ibmw_selfdev_warnings(${mod})
    if(variant STREQUAL "bsymbolic")
      target_link_options(${mod} PRIVATE -Wl,-Bsymbolic)
    elseif(variant STREQUAL "vscript")
      target_link_options(${mod} PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/dso_module.map)
    endif()
  endforeach()

  add_test(NAME ibmw_coro_dso_deepbind_${opt}
           COMMAND ibmw_coro_dso_main_linked $<TARGET_FILE:ibmw_coro_dso_module_plain_${opt}> deepbind ${_coro_dso_iterations})
  add_test(NAME ibmw_coro_dso_bsymbolic_${opt}
           COMMAND ibmw_coro_dso_main_linked $<TARGET_FILE:ibmw_coro_dso_module_bsymbolic_${opt}> local ${_coro_dso_iterations})
  add_test(NAME ibmw_coro_dso_bsymbolic_deepbind_${opt}
           COMMAND ibmw_coro_dso_main_linked $<TARGET_FILE:ibmw_coro_dso_module_bsymbolic_${opt}> deepbind ${_coro_dso_iterations})
  add_test(NAME ibmw_coro_dso_vscript_${opt}
           COMMAND ibmw_coro_dso_main_linked $<TARGET_FILE:ibmw_coro_dso_module_vscript_${opt}> deepbind ${_coro_dso_iterations})
  add_test(NAME ibmw_coro_dso_dlopen_only_${opt}
           COMMAND ibmw_coro_dso_main_dlopen $<TARGET_FILE:ibmw_coro_dso_module_plain_${opt}> deepbind ${_coro_dso_iterations})
  add_test(NAME ibmw_coro_dso_pre_thread_${opt}
           COMMAND ibmw_coro_dso_main_linked $<TARGET_FILE:ibmw_coro_dso_module_plain_${opt}> deepbind ${_coro_dso_iterations}
                   pre-thread)
  add_test(NAME ibmw_coro_dso_dlopen_only_pre_thread_${opt}
           COMMAND ibmw_coro_dso_main_dlopen $<TARGET_FILE:ibmw_coro_dso_module_plain_${opt}> deepbind ${_coro_dso_iterations}
                   pre-thread)
  set_tests_properties(
    ibmw_coro_dso_deepbind_${opt} ibmw_coro_dso_bsymbolic_${opt} ibmw_coro_dso_bsymbolic_deepbind_${opt}
    ibmw_coro_dso_vscript_${opt} ibmw_coro_dso_dlopen_only_${opt}
    ibmw_coro_dso_pre_thread_${opt} ibmw_coro_dso_dlopen_only_pre_thread_${opt}
    PROPERTIES PASS_REGULAR_EXPRESSION "PASS" FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
