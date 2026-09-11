# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get gflags ...")

set(gflags_DOWNLOAD_URL
    "https://github.com/gflags/gflags/archive/v2.2.2.tar.gz"
    CACHE STRING "")

if(gflags_LOCAL_SOURCE)
  FetchContent_Declare(
    gflags
    SOURCE_DIR ${gflags_LOCAL_SOURCE}
    OVERRIDE_FIND_PACKAGE)
else()
  FetchContent_Declare(
    gflags
    URL ${gflags_DOWNLOAD_URL}
    URL_HASH SHA256=34af2f15cf7367513b352bdcd2493ab14ce43692d2dcd9dfc499492966c64dcf
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    OVERRIDE_FIND_PACKAGE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_gflags)
  FetchContent_GetProperties(gflags)
  if(NOT gflags_POPULATED)
    FetchContent_Populate(gflags)

    set(BUILD_TESTING OFF)

    # Keep FetchContent sources immutable while pinning the intended pkg-config
    # installation directory through gflags' cache setting.
    set(PKGCONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/pkgconfig" CACHE PATH "" FORCE)
    add_subdirectory(${gflags_SOURCE_DIR} ${gflags_BINARY_DIR})

  endif()
endfunction()

get_gflags()

# import targets:
# gflags::gflags
