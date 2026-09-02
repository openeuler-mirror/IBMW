# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get cpptoml...")

set(cpptoml_DOWNLOAD_URL
    "https://github.com/skystrife/cpptoml/archive/refs/tags/v0.1.0.tar.gz"
    CACHE STRING "")

if(cpptoml_LOCAL_SOURCE)
  FetchContent_Declare(
    cpptoml
    SOURCE_DIR ${cpptoml_LOCAL_SOURCE}
    OVERRIDE_FIND_PACKAGE)
else()
  FetchContent_Declare(
    cpptoml
    URL ${cpptoml_DOWNLOAD_URL}
    URL_HASH SHA256=11b1c07132d23e85071cef3d0fe9bc605de731fcd1b1c3f3d43ed09a6fd7a850
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    PATCH_COMMAND
      patch --forward --strip=1
      --input=${CMAKE_CURRENT_LIST_DIR}/patches/cpptoml-0.1.0-limits.patch
    OVERRIDE_FIND_PACKAGE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_cpptoml)
  FetchContent_GetProperties(cpptoml)
  if(NOT cpptoml_POPULATED)
    FetchContent_Populate(cpptoml)

    set(CPPTOML_BUILD_EXAMPLES OFF CACHE BOOL "Build cpptoml examples" FORCE)

    # Suppress cpptoml's export(PACKAGE) write.
    set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)
    add_subdirectory(${cpptoml_SOURCE_DIR} ${cpptoml_BINARY_DIR})

  endif()
endfunction()

get_cpptoml()
