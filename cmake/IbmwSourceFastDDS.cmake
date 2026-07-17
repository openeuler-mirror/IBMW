# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(IBMW_FASTDDS_SOURCE_URL
    "https://github.com/eProsima/Fast-DDS/archive/refs/tags/v3.2.2.tar.gz"
    CACHE STRING "Fast DDS source archive URL for IBMW source-prefix builds.")
set(IBMW_FASTDDS_SOURCE_DIR
    ""
    CACHE PATH "Optional local Fast DDS source tree for IBMW source-prefix builds.")

ibmw_source_prefix_path_maybe_arg(_ibmw_asio_include_arg Asio_INCLUDE_DIR "${IBMW_THIRD_PARTY_SOURCE_PREFIX}/include")

set(_ibmw_fastdds_cmake_args
    -DCMAKE_PREFIX_PATH=${IBMW_THIRD_PARTY_SOURCE_PREFIX}
    -DAsio_DIR=${IBMW_THIRD_PARTY_SOURCE_PREFIX}/lib/cmake/asio
    ${_ibmw_asio_include_arg}
    -DTHIRDPARTY=OFF
    -DTHIRDPARTY_UPDATE=OFF
    -DTHIRDPARTY_fastcdr=OFF
    -DTHIRDPARTY_Asio=OFF
    -DTHIRDPARTY_TinyXML2=OFF
    -DCOMPILE_TOOLS=OFF
    -DSECURITY=OFF
    -DSQLITE3_SUPPORT=OFF
    -DBUILD_DOCUMENTATION=OFF
    -DBUILD_EXAMPLES=OFF
    -DBUILD_TESTING=OFF
    -DCOMPILE_EXAMPLES=OFF)

set(_ibmw_fastdds_ohos_arm64 FALSE)
if(CMAKE_CROSSCOMPILING AND DEFINED OHOS_PLATFORM AND "${OHOS_PLATFORM}" STREQUAL "OHOS")
  if((DEFINED OHOS_ARCH AND "${OHOS_ARCH}" STREQUAL "arm64-v8a") OR
     CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(_ibmw_fastdds_ohos_arm64 TRUE)
  endif()
endif()

if(_ibmw_fastdds_ohos_arm64)
  # OHOS AArch64 ABI workaround: an external callee (OHOS musl / compiler-rt /
  # CFI/signal shim) reachable from XMLProfileManager::loadXMLFile fails to
  # preserve callee-saved x22 across the call chain, so Fast DDS's -O2 codegen
  # (which legally caches a std::stringstream base in x22 across `bl
  # XMLParser::loadXML`) reads x22==0 and SIGSEGVs at create_participant ->
  # load_profiles. Reserving x22 removes it from the allocatable pool so no TU
  # can cache a live value there across the offending call; this is robust
  # against both register-clobber and saved-slot-smash failure modes. Must be
  # applied to every Fast DDS TU in the OHOS ARM64 source-prefix lane.
  list(APPEND _ibmw_fastdds_cmake_args
       "-DCMAKE_CXX_FLAGS=-ffixed-x22 -Wno-unused-command-line-argument")

  # OHOS AArch64 dual-libc++ EH/RTTI isolation (ROUND 15 root cause).
  #
  # The OHOS musl loader force-maps the system "__h2" libc++
  # (/system/lib64/chipset-sdk-sp/libc++.so) ahead of the runtime "__n1"
  # libc++_shared.so. Both export identical un-namespaced exception/RTTI runtime
  # (__cxa_*, operator new/delete, __gxx_personality_v0, _ZTISt12length_error,
  # _ZTISt9bad_alloc, ...). Without symbolic binding, libfastdds.so resolves its
  # exception-throwing / type_info runtime to the first-loaded __h2 libc++ while
  # its std::__n1 containers run on __n1, producing a cross-namespace EH/RTTI
  # mismatch that surfaces as a spurious std::length_error("vector") /
  # std::bad_alloc inside create_participant -> PDP discovery construction.
  #
  # REMEDY R-A (static-fold libc++). The EH/RTTI/operator-new symbols (__cxa_*,
  # _Znwm, _ZTISt*, __gxx_personality_v0) are UNDEFINED imports in libfastdds, so
  # a version-script `local:` and -Bsymbolic cannot relocalize them (those only
  # act on DEFINED symbols), and ld.lld 15 cannot emit DF_1_GROUP (-Bgroup is
  # rejected), so import-scoping is impossible on this toolchain. The only working
  # mechanism is to statically fold the RT __n1 libc++ into libfastdds so those
  # symbols become DEFINED inside it; then they self-resolve and are hidden from
  # the first-loaded system __h2 libc++.
  #
  #  -static-libstdc++                 : link libc++_static.a + libc++abi.a +
  #       libunwind.a into libfastdds so __cxa_*, operator new/delete, _ZTISt*,
  #       __gxx_personality_v0 become DEFINED (T/W) inside the .so rather than
  #       U imports bound to the globally-first __h2 libc++.
  #  -Bsymbolic / -Bsymbolic-functions : now EFFECTIVE - bind the now-DEFINED
  #       libc++ references to libfastdds's own copies, defeating interposition.
  #  --exclude-libs,ALL                : now EFFECTIVE - localize the symbols
  #       pulled from the static libc++ archives so they are not re-exported and
  #       cannot be interposed by __h2.
  #  --version-script                  : force the un-namespaced EH/RTTI runtime
  #       symbols local (now that they are DEFINED), belt-and-suspenders with
  #       --exclude-libs,ALL.
  #
  # Do NOT add -fvisibility=hidden here: combining it with these link flags
  # breaks the loadXMLFile path by hiding symbols Fast DDS must export.
  set(_ibmw_fastdds_version_script "${CMAKE_CURRENT_LIST_DIR}/fastdds_ohos_symbol_isolation.map")
  set(_ibmw_fastdds_link_flags
      "-static-libstdc++ -Wl,-Bsymbolic -Wl,-Bsymbolic-functions -Wl,--exclude-libs,ALL -Wl,--version-script,${_ibmw_fastdds_version_script}")
  list(APPEND _ibmw_fastdds_cmake_args
       "-DCMAKE_SHARED_LINKER_FLAGS=${_ibmw_fastdds_link_flags}"
       "-DCMAKE_MODULE_LINKER_FLAGS=${_ibmw_fastdds_link_flags}")
endif()

ibmw_add_source_prefix_external_project(ibmw_source_fastdds
  URL "${IBMW_FASTDDS_SOURCE_URL}"
  SOURCE_DIR "${IBMW_FASTDDS_SOURCE_DIR}"
  DEPENDS ibmw_source_tinyxml2 ibmw_source_foonathan_memory ibmw_source_asio ibmw_source_fastcdr
  CMAKE_ARGS
    ${_ibmw_fastdds_cmake_args})
