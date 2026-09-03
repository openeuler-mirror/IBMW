if(NOT DEFINED BUILD_DIR OR NOT IS_ABSOLUTE "${BUILD_DIR}")
  message(FATAL_ERROR "BUILD_DIR must be an absolute path")
endif()
if(NOT DEFINED INSTALL_PREFIX OR NOT IS_ABSOLUTE "${INSTALL_PREFIX}")
  message(FATAL_ERROR "INSTALL_PREFIX must be an absolute path")
endif()
if(NOT DEFINED COMPONENT OR COMPONENT STREQUAL "")
  message(FATAL_ERROR "COMPONENT is required")
endif()

file(REMOVE_RECURSE "${INSTALL_PREFIX}")
if(EXISTS "${INSTALL_PREFIX}")
  message(FATAL_ERROR "failed to remove stale install prefix: ${INSTALL_PREFIX}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
    --prefix "${INSTALL_PREFIX}" --component "${COMPONENT}"
  RESULT_VARIABLE install_result)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "component install failed with exit code ${install_result}")
endif()
