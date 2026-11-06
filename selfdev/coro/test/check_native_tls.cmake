# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# IBMW_SELFDEV_CORO_NATIVE_TLS: LIB must not import __emutls_get_address
# (the slot would still be emulated, e.g. after an LTO link without the flag).
# Assumes the emutls runtime is a shared import, as with the OHOS SDK
# (libc++/compiler-rt); a toolchain linking it statically would pass.
# Usage: cmake -DNM=<nm> -DLIB=<library> -P check_native_tls.cmake

execute_process(
  COMMAND ${NM} -D --undefined-only ${LIB}
  OUTPUT_VARIABLE nm_out
  ERROR_VARIABLE nm_err
  RESULT_VARIABLE nm_rc)
if(NOT nm_rc EQUAL 0)
  message(FATAL_ERROR "${NM} -D --undefined-only ${LIB} failed (${nm_rc}): ${nm_err}")
endif()
if(nm_out MATCHES "__emutls_get_address")
  message(FATAL_ERROR "FAIL: ${LIB} imports __emutls_get_address: TLS is still emulated")
endif()
message(STATUS "PASS")
