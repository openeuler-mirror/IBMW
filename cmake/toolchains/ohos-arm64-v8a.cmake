# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

if(NOT DEFINED OHOS_SDK_NATIVE_ROOT)
  if(DEFINED ENV{OHOS_SDK_NATIVE_ROOT})
    set(_ibmw_ohos_sdk_native_root_default "$ENV{OHOS_SDK_NATIVE_ROOT}")
  else()
    set(_ibmw_ohos_sdk_native_root_default "")
  endif()
else()
  set(_ibmw_ohos_sdk_native_root_default "${OHOS_SDK_NATIVE_ROOT}")
endif()
set(OHOS_SDK_NATIVE_ROOT "${_ibmw_ohos_sdk_native_root_default}" CACHE PATH
    "OpenHarmony native SDK root")
unset(_ibmw_ohos_sdk_native_root_default)
set(OHOS_ARCH "arm64-v8a" CACHE STRING "OpenHarmony target architecture")
set(OHOS_PLATFORM "OHOS" CACHE STRING "OpenHarmony target platform")
set(OHOS_STL "c++_shared" CACHE STRING "OpenHarmony STL runtime")
set(CMAKE_SYSTEM_PROCESSOR "aarch64" CACHE STRING "Target processor")

set(_ibmw_ohos_toolchain "${OHOS_SDK_NATIVE_ROOT}/build/cmake/ohos.toolchain.cmake")
if(NOT EXISTS "${_ibmw_ohos_toolchain}")
  message(FATAL_ERROR
    "OHOS toolchain file not found: ${_ibmw_ohos_toolchain}. "
    "Set OHOS_SDK_NATIVE_ROOT to your OpenHarmony SDK native directory.")
endif()

set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    ${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES}
    OHOS_SDK_NATIVE_ROOT
    OHOS_ARCH
    OHOS_PLATFORM
    OHOS_STL)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

# Enable ELF DT_SONAME generation for OHOS shared libraries without
# switching CMake to versioned SONAME/file-name layouts.
set(CMAKE_SHARED_LIBRARY_SONAME_C_FLAG "-Wl,-soname," CACHE STRING
    "SONAME linker flag for C shared libraries on OHOS")
set(CMAKE_SHARED_LIBRARY_SONAME_CXX_FLAG "-Wl,-soname," CACHE STRING
    "SONAME linker flag for C++ shared libraries on OHOS")
set(CMAKE_PLATFORM_NO_VERSIONED_SONAME ON CACHE BOOL
    "Use unversioned SONAME behavior for OHOS shared libraries")
# CMake has no OHOS platform module, so ENABLE_EXPORTS adds nothing without these.
set(CMAKE_EXE_EXPORTS_C_FLAG "-Wl,--export-dynamic" CACHE STRING
    "Linker flag exporting executable symbols (ENABLE_EXPORTS) on OHOS")
set(CMAKE_EXE_EXPORTS_CXX_FLAG "-Wl,--export-dynamic" CACHE STRING
    "Linker flag exporting executable symbols (ENABLE_EXPORTS) on OHOS")

include("${_ibmw_ohos_toolchain}")
