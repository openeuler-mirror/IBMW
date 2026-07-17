# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get TBB ...")

set(tbb_DOWNLOAD_URL
    "https://github.com/oneapi-src/oneTBB/archive/v2021.13.0.tar.gz"
    CACHE STRING "")

if(tbb_LOCAL_SOURCE)
  FetchContent_Declare(
    tbb
    SOURCE_DIR ${tbb_LOCAL_SOURCE}
    OVERRIDE_FIND_PACKAGE)
else()
  FetchContent_Declare(
    tbb
    URL ${tbb_DOWNLOAD_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    OVERRIDE_FIND_PACKAGE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_tbb)
  FetchContent_GetProperties(tbb)
  if(NOT tbb_POPULATED)
    set(TBB_TEST OFF)

    set(TBB_DIR "")

    set(TBB_INSTALL ${IBMW_INSTALL})

    set(TBB_STRICT OFF)

    FetchContent_MakeAvailable(tbb)

    # Build-tree export: same pattern as GetYamlCpp.cmake. Upstream oneTBB
    # owns its install(EXPORT TBBTargets) when TBB_INSTALL=ON, but does NOT
    # call export() — so a build-tree find_package(TBB) cannot resolve targets.
    # We add export() here plus copy upstream-generated TBBConfig(.Version).cmake
    # into build-tree-config so find_package(TBB CONFIG) works against a pure
    # build tree. See docs/contributing/testing.md.
    if(IBMW_INSTALL AND TARGET tbb)
      export(
        EXPORT TBBTargets
        NAMESPACE TBB::
        FILE "${CMAKE_BINARY_DIR}/build-tree-config/lib/cmake/TBB/TBBTargets.cmake")
      configure_file(
        "${tbb_BINARY_DIR}/TBBConfig.cmake"
        "${CMAKE_BINARY_DIR}/build-tree-config/lib/cmake/TBB/TBBConfig.cmake"
        COPYONLY)
      configure_file(
        "${tbb_BINARY_DIR}/TBBConfigVersion.cmake"
        "${CMAKE_BINARY_DIR}/build-tree-config/lib/cmake/TBB/TBBConfigVersion.cmake"
        COPYONLY)
    endif()
  endif()
endfunction()

get_tbb()

# import targets:
# TBB::tbb
