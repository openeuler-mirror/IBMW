# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(AddWerror)

if(IBMW_BUILD_RETAINED_LOAN_E2E)
  add_executable(retained_loan_e2e retained_loan_e2e.cc)

  target_link_libraries(retained_loan_e2e PRIVATE
    ibmw::core::engine
    ibmw::core::reporter_host_abi
    ibmw::extensions::iceoryx_ext
    iceoryx_posh::iceoryx_posh)

  add_werror(retained_loan_e2e)

  # Reporter V1 is intentionally resolved from the metrics-owner executable.
  # Exporting the host query symbol prevents a directly linked extension from
  # becoming a second reporter owner.
  set_target_properties(retained_loan_e2e PROPERTIES ENABLE_EXPORTS ON)
  if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    # The Reporter host ABI requires export-dynamic. Our OHOS toolchain makes
    # ENABLE_EXPORTS add it; kept for trees configured with another toolchain.
    target_link_options(retained_loan_e2e PRIVATE
      "LINKER:--export-dynamic")
    add_custom_command(TARGET retained_loan_e2e POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
        -DIBMW_NM=${CMAKE_NM}
        -DIBMW_EXECUTABLE=$<TARGET_FILE:retained_loan_e2e>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_reporter_export.cmake"
      VERBATIM)
  endif()

  include(IbmwIceoryxRouDi)
  ibmw_add_iceoryx_roudi_dependency(retained_loan_e2e)
  ibmw_generate_iceoryx_roudi_path(
    "${CMAKE_BINARY_DIR}/retained_loan_e2e_roudi_path.txt")
endif()

if(IBMW_BUILD_SHM_RETAINED_LOAN_E2E)
  # The same workloads on the self-developed shm driver: shm_ext is linked in
  # directly (ShmBus + ShmTransportDriver), no RouDi. IBMW_SHM_ROOT and
  # IBMW_SHM_DOMAIN select the discovery root of a run.
  add_executable(shm_retained_loan_e2e retained_loan_e2e.cc)
  target_compile_definitions(shm_retained_loan_e2e PRIVATE IBMW_RETAINED_E2E_SHM)
  target_link_libraries(shm_retained_loan_e2e PRIVATE
    ibmw::core::engine
    ibmw::core::reporter_host_abi
    ibmw::extensions::shm_ext)
  add_werror(shm_retained_loan_e2e)

  # As above: the executable is the Reporter V1 host, shm_ext resolves the
  # query from it.
  set_target_properties(shm_retained_loan_e2e PROPERTIES ENABLE_EXPORTS ON)
  if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
    target_link_options(shm_retained_loan_e2e PRIVATE
      "LINKER:--export-dynamic")
    add_custom_command(TARGET shm_retained_loan_e2e POST_BUILD
      COMMAND "${CMAKE_COMMAND}"
        -DIBMW_NM=${CMAKE_NM}
        -DIBMW_EXECUTABLE=$<TARGET_FILE:shm_retained_loan_e2e>
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_reporter_export.cmake"
      VERBATIM)
  endif()

  # Both workloads run to completion on a private shm root and their Reporter
  # V1 counters are checked; without Reporter V1 there is nothing to check.
  if(IBMW_BUILD_TESTS AND IBMW_ENABLE_REPORTER_V1 AND
     CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME shm_retained_loan_e2e_metrics
      COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check_shm_metrics.sh"
              "$<TARGET_FILE_DIR:shm_retained_loan_e2e>"
              "${CMAKE_CURRENT_BINARY_DIR}/shm_retained_loan_e2e_metrics")
    set_tests_properties(shm_retained_loan_e2e_metrics PROPERTIES
      ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}"
      TIMEOUT 120)
  endif()
endif()
