# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
#
# CMake target:  ibmw_extensions_iceoryx_ext   (SHARED)
# Alias        :  ibmw::extensions::iceoryx_ext
#
# This single SHARED target intentionally serves *both* roles that dds_ext
# splits across two targets (dds_extension SHARED + dds_ext INTERFACE):
#   1. Plugin loaded at runtime by the engine via libdl (extensions: config)
#   2. Direct-link target for in-tree samples *and* out-of-tree consumers
#      (notably ibmw-gen generated `loaned_publisher` modules with
#      transport=iceoryx) which need IceoryxTransportDriver typeinfo +
#      IBMW_ICEORYX_EXPORT'ed member function symbols (EnsurePodTopic,
#      Send/Loan APIs) plus the public iceoryx_ext headers.
# A separate INTERFACE wrapper is unnecessary because:
#   - Public headers are already exposed via PUBLIC include below
#   - iceoryx_posh transitive deps are already exposed via PUBLIC link below
#   - The SHARED .so itself is what gen consumers need to link
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Set file collection
file(GLOB_RECURSE src ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
list(REMOVE_ITEM src ${test_files})

# Public headers (installed + attached as INTERFACE FILE_SET HEADERS so
# downstream consumers get them via the exported target).
file(GLOB iceoryx_ext_public_headers ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

# Add target
add_library(${CUR_TARGET_NAME} SHARED)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Set source file of target
target_sources(${CUR_TARGET_NAME} PRIVATE ${src})

# Attach public headers as a FILE_SET so `install(... FILE_SET HEADERS ...)`
# can re-export them and downstream `find_package(ibmw)` consumers can
# `#include "iceoryx_ext/iceoryx_transport_driver.h"` from the install tree.
target_sources(${CUR_TARGET_NAME}
  PUBLIC FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
  FILES ${iceoryx_ext_public_headers}
)

# Set include path of target
#
# PUBLIC: benchmark/sample executables (e.g. transport_benchmark/iox_loaned_pod)
# *and* ibmw-gen `loaned_publisher` modules link this .so directly to gain
# access to IceoryxTransportDriver and the header-only IoxStylePublisher /
# IoxRawPodPublisher / IoxLoanedSample templates (which call into
# IceoryxTransportDriver methods that are explicitly exported via
# IBMW_ICEORYX_EXPORT in iceoryx_transport_driver.h).
target_include_directories(
  ${CUR_TARGET_NAME}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include/ibmw>)

# Set link libraries of target
#
# iceoryx_posh is PRIVATE because the iceoryx::iceoryx_posh target is not
# part of ibmw's install export set (iceoryx is FetchContent'd in-tree and
# its own install/export step doesn't participate in INSTALL_CONFIG_NAME).
# Although iceoryx_ext public headers (iceoryx_bus.h) transitively #include
# <iceoryx_posh/popo/...>, downstream consumers (samples + ibmw-gen
# loaned_publisher modules) MUST link iceoryx_posh::iceoryx_posh explicitly
# alongside ibmw::extensions::iceoryx_ext. See
# samples/cpp/transport_benchmark/iox_loaned_pod/managed/.../CMakeLists.txt
# for the canonical pattern; cmake_gen.py replicates it for generated code.
#
# ibmw::api::cpp + ibmw::core::engine are PUBLIC so downstream out-of-tree
# consumers (ibmw-gen loaned_publisher modules) inherit the framework headers
# (e.g. transport/loaned_sample.h, engine/transport/transport_driver.h) that
# iceoryx_transport_driver.h transitively #includes.
target_link_libraries(
  ${CUR_TARGET_NAME}
  PRIVATE ibmw::core::extension_api
          iceoryx_posh::iceoryx_posh
  PUBLIC  ibmw::api::cpp
          ibmw::core::engine)

# Add -Werror option
include(AddWerror)
add_werror(${CUR_TARGET_NAME})

# Set installation of target
#
# EXPORT to ${INSTALL_CONFIG_NAME} so downstream `find_package(ibmw)` can
# resolve the SHARED lib's typeinfo + IBMW_ICEORYX_EXPORT'ed member symbols
# (required by ibmw-gen loaned_publisher modules performing
# dynamic_cast<IceoryxTransportDriver*> + driver_->EnsurePodTopic() calls).
#
# Headers re-exported via FILE_SET so generated code can
# `#include "iceoryx_ext/..."` from the install tree.
if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME}
    PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  install(TARGETS ${CUR_TARGET_NAME}
    EXPORT ${INSTALL_CONFIG_NAME}
    LIBRARY DESTINATION bin
    ARCHIVE DESTINATION bin
    FILE_SET HEADERS DESTINATION include/ibmw/iceoryx_ext)
endif()

# Set test of target
if(IBMW_BUILD_TESTS AND test_files)
  add_gtest_target(TEST_TARGET ${CUR_TARGET_NAME} TEST_SRC ${test_files})
endif()

# Set misc of target
set_target_properties(${CUR_TARGET_NAME} PROPERTIES OUTPUT_NAME "ibmw_iceoryx_transport")

# Note: the iceoryx_ext .so is built with the project-global hidden
# visibility preset, but a small set of symbols required by sample/benchmark
# executables (IceoryxTransportDriver typeinfo + vtable + methods) are
# explicitly exported via the IBMW_ICEORYX_EXPORT class-level attribute in
# iceoryx_transport_driver.h. This keeps the dlopen entry-point story
# unchanged (engine still loads the .so via libdl) while giving direct-link
# consumers a tightly-scoped public ABI -- no parallel STATIC duplicate of
# the iceoryx integration is needed (avoids in-process iceoryx-singleton
# conflicts seen with the static-counterpart approach).
