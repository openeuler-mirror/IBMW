# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get libunifex ...")

set(libunifex_DOWNLOAD_URL
    "https://github.com/facebookexperimental/libunifex/archive/591ec09e7d51858ad05be979d4034574215f5971.tar.gz"
    CACHE STRING "")

if(libunifex_LOCAL_SOURCE)
  FetchContent_Declare(
    libunifex
    SOURCE_DIR ${libunifex_LOCAL_SOURCE}
    OVERRIDE_FIND_PACKAGE)
else()
  FetchContent_Declare(
    libunifex
    URL ${libunifex_DOWNLOAD_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    OVERRIDE_FIND_PACKAGE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_libunifex)
  FetchContent_GetProperties(libunifex)
  if(NOT libunifex_POPULATED)
    set(UNIFEX_BUILD_EXAMPLES OFF)

    FetchContent_MakeAvailable(libunifex)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(unifex PRIVATE -Wno-unused-but-set-variable)

      if(CMAKE_CROSSCOMPILING AND DEFINED OHOS_PLATFORM AND NOT OHOS_PLATFORM STREQUAL "")
        target_compile_options(unifex PRIVATE -Wno-unused-command-line-argument)
      endif()
    endif()

    add_library(unifex::unifex ALIAS unifex)

    # Build-tree export: differs from yaml-cpp/TBB pattern. Upstream libunifex
    # (`_deps/libunifex-src/source/CMakeLists.txt:93-96`) uses
    # `install(EXPORT unifexTargets FILE unifexConfig.cmake NAMESPACE unifex::)`
    # — i.e. the install-tree exports a SINGLE file named unifexConfig.cmake
    # which IS the targets file (no separate Config / ConfigVersion files).
    # Mirror that here: one export() call writing unifexConfig.cmake into
    # build-tree-config; no configure_file needed. There is no upstream
    # ConfigVersion file to copy. See docs/contributing/testing.md.
    if(IBMW_INSTALL AND TARGET unifex)
      export(
        EXPORT unifexTargets
        NAMESPACE unifex::
        FILE "${CMAKE_BINARY_DIR}/build-tree-config/lib/cmake/unifex/unifexConfig.cmake")
    endif()
  endif()
endfunction()

get_libunifex()

# import targets:
# unifex::unifex
