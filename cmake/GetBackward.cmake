# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get backward ...")

set(backward_DOWNLOAD_URL
    "https://github.com/bombela/backward-cpp/archive/refs/tags/v1.6.tar.gz"
    CACHE STRING "")

if(backward_LOCAL_SOURCE)
  FetchContent_Declare(
    backward
    SOURCE_DIR ${backward_LOCAL_SOURCE}
    OVERRIDE_FIND_PACKAGE)
else()
  FetchContent_Declare(
    backward
    URL ${backward_DOWNLOAD_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    OVERRIDE_FIND_PACKAGE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_backward)
  FetchContent_GetProperties(backward)
  if(NOT backward_POPULATED)
    FetchContent_Populate(backward)

    # Include BackwardConfig.cmake to get definitions and libraries
    include(${backward_SOURCE_DIR}/BackwardConfig.cmake)

    # Create a real (non-IMPORTED) INTERFACE library so it can be installed
    # and exported. This follows the same pattern as GetAsio.cmake.
    # Note: The upstream BackwardConfig.cmake may have already created a
    # Backward::Backward IMPORTED target, so we use a different base name
    # and set EXPORT_NAME to Backward::Backward.
    if(NOT TARGET backward_iface)
      add_library(backward_iface INTERFACE)

      target_include_directories(
        backward_iface
        INTERFACE
          $<BUILD_INTERFACE:${backward_SOURCE_DIR}>
          $<INSTALL_INTERFACE:include/backward>)

      if(BACKWARD_DEFINITIONS)
        target_compile_definitions(backward_iface INTERFACE ${BACKWARD_DEFINITIONS})
      endif()

      if(BACKWARD_HAS_EXTERNAL_LIBRARIES)
        target_link_libraries(backward_iface INTERFACE ${BACKWARD_LIBRARIES})
      endif()

      # If the upstream BackwardConfig.cmake already created Backward::Backward
      # as an IMPORTED target, remove it so we can use the installable target.
      # This is necessary because IMPORTED targets cannot be exported/installed.
      if(TARGET Backward::Backward)
        # We can't remove a target in CMake, but we can override its usage
        # by making engine link against backward_iface instead.
      endif()

      # Install backward.hpp header
      install(
        FILES ${backward_SOURCE_DIR}/backward.hpp
        DESTINATION include/backward)

      set_property(TARGET backward_iface PROPERTY EXPORT_NAME Backward::Backward)
      if(IBMW_INSTALL)
        install(
          TARGETS backward_iface
          EXPORT backward-config
          INCLUDES DESTINATION include/backward)

        install(EXPORT backward-config DESTINATION lib/cmake/backward)

        # Build-tree export: mirror GetAsio.cmake. Writes backward-config.cmake
        # into ${CMAKE_BINARY_DIR}/build-tree-config/lib/cmake/backward/ so the
        # ibmw build-tree package config can find_dependency(backward) without
        # requiring an install step. See top-level CMakeLists.txt and
        # docs/contributing/testing.md.
        export(
          EXPORT backward-config
          FILE "${CMAKE_BINARY_DIR}/build-tree-config/lib/cmake/backward/backward-config.cmake")
      endif()
    endif()
  endif()
endfunction()

get_backward()

# import targets:
# backward_iface (exported as Backward::Backward)
# Note: During build, link against backward_iface (alias not available due to
# upstream BackwardConfig.cmake creating Backward::Backward as IMPORTED).
# After install, find_package(backward) provides Backward::Backward.
