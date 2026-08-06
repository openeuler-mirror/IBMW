# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get iceoryx...")

if(IBMW_ICEORYX_USE_PACKAGE)
  find_package(iceoryx_posh CONFIG REQUIRED)
  return()
endif()

set(iceoryx_DOWNLOAD_URL
    "https://github.com/eclipse-iceoryx/iceoryx/archive/refs/tags/v2.95.4.tar.gz"
    CACHE STRING "")

set(DOWNLOAD_TOML_LIB OFF)

if(iceoryx_LOCAL_SOURCE)
  FetchContent_Declare(
    iceoryx
    SOURCE_DIR ${iceoryx_LOCAL_SOURCE}
    OVERRIDE_FIND_PACKAGE)
else()
  FetchContent_Declare(
    iceoryx
    URL ${iceoryx_DOWNLOAD_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    OVERRIDE_FIND_PACKAGE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_iceoryx)
  FetchContent_GetProperties(iceoryx)
  if(NOT iceoryx_POPULATED)

    FetchContent_Populate(iceoryx)

    set(INTROSPECTION ON)

    add_subdirectory(${iceoryx_SOURCE_DIR}/iceoryx_meta ${iceoryx_BINARY_DIR})

  endif()
endfunction()

get_iceoryx()
