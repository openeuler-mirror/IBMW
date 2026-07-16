# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Get all the files in folder
file(GLOB_RECURSE IBMW_CLI_SRC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/ibmw_cli/*")

add_custom_target(
  ibmw_cli ALL
  COMMAND ${Python3_EXECUTABLE} -m PyInstaller ${CMAKE_CURRENT_SOURCE_DIR}/ibmw_cli/ibmw_cli.spec
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "${Python3_EXECUTABLE} -m PyInstaller ${CMAKE_CURRENT_SOURCE_DIR}/ibmw_cli/ibmw_cli.spec"
  DEPENDS ${IBMW_CLI_SRC_FILES})

if(IBMW_INSTALL)
  install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/dist/
    DESTINATION bin
    USE_SOURCE_PERMISSIONS)
endif()
