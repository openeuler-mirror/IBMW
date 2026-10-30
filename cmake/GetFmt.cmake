# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get fmt ...")

set(fmt_DOWNLOAD_URL
    "https://github.com/fmtlib/fmt/archive/10.2.1.tar.gz"
    CACHE STRING "")
set(fmt_DOWNLOAD_SHA256
    "1250e4cc58bf06ee631567523f48848dc4596133e163f02615c97f78bab6c811"
    CACHE STRING "SHA256 of fmt_DOWNLOAD_URL (update both together)")

if(fmt_LOCAL_SOURCE)
  FetchContent_Declare(
    fmt
    SOURCE_DIR ${fmt_LOCAL_SOURCE}
    OVERRIDE_FIND_PACKAGE)
else()
  FetchContent_Declare(
    fmt
    URL ${fmt_DOWNLOAD_URL}
    URL_HASH SHA256=${fmt_DOWNLOAD_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    OVERRIDE_FIND_PACKAGE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_fmt)
  FetchContent_GetProperties(fmt)
  if(NOT fmt_POPULATED)
    set(FMT_MASTER_PROJECT OFF)

    set(FMT_INSTALL ${IBMW_INSTALL})

    FetchContent_MakeAvailable(fmt)
  endif()
endfunction()

get_fmt()

# import targets:
# fmt::fmt
