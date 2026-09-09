# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

add_executable(retained_loan_e2e retained_loan_e2e.cc)

target_link_libraries(retained_loan_e2e PRIVATE
  ibmw::core::engine
  ibmw::core::reporter_host_abi
  ibmw::extensions::iceoryx_ext
  iceoryx_posh::iceoryx_posh)

include(AddWerror)
add_werror(retained_loan_e2e)

# Reporter V1 is intentionally resolved from the metrics-owner executable.
# Exporting the host query symbol prevents a directly linked extension from
# becoming a second reporter owner.
set_target_properties(retained_loan_e2e PROPERTIES ENABLE_EXPORTS ON)
if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  # CMake has no built-in OHOS platform module, so ENABLE_EXPORTS does not add
  # the export-dynamic linker option that the Reporter host ABI requires.
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
