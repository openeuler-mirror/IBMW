# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_root_namespace("ibmw")

add_subdirectory(base)
add_subdirectory(internal)
add_subdirectory(api)

if(IBMW_BUILD_RUNTIME)
  add_subdirectory(core)
endif()

add_subdirectory(tools)

if(IBMW_BUILD_RUNTIME)
  add_subdirectory(extensions)

  # Each enabled provider contributes to the same deployment manifest.
  get_property(cli_provider_targets GLOBAL PROPERTY IBMW_CLI_PROVIDER_TARGETS)
  set(cli_build_manifest "providers:\n")
  set(cli_install_manifest "providers:\n")
  set(cli_stage_manifest "providers:\n")
  foreach(provider IN LISTS cli_provider_targets)
    add_dependencies(ibmw_core_main ${provider})
    string(APPEND cli_build_manifest
           "  - library: \"$<TARGET_FILE:${provider}>\"\n")
    string(APPEND cli_install_manifest
           "  - library: \"../../bin/$<TARGET_FILE_NAME:${provider}>\"\n")
    string(APPEND cli_stage_manifest
           "  - library: \"lib/$<TARGET_FILE_NAME:${provider}>\"\n")
  endforeach()
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/cli.yaml CONTENT "${cli_build_manifest}")
  file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/cli-install.yaml
       CONTENT "${cli_install_manifest}")
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/cli-stage.yaml
       CONTENT "${cli_stage_manifest}")
  if(IBMW_INSTALL)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cli-install.yaml
            DESTINATION share/ibmw RENAME cli.yaml)
  endif()
endif()

add_subdirectory(schema)

if(IBMW_BUILD_EXAMPLES OR IBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES OR
   IBMW_BUILD_RETAINED_LOAN_E2E OR IBMW_BUILD_DDS_XML_VALIDATION)
  add_subdirectory(samples)
endif()
