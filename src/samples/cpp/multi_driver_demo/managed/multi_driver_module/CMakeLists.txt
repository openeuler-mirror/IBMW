# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

file(GLOB_RECURSE head_files ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE src ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB_RECURSE test_files ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc)
list(REMOVE_ITEM src ${test_files})

# DDS + Iceoryx variant.
if(IBMW_BUILD_ICEORYX_EXTENSION)
  add_library(${CUR_TARGET_NAME} STATIC)
  add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

  target_sources(${CUR_TARGET_NAME} PRIVATE ${src})

  target_include_directories(
    ${CUR_TARGET_NAME}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)

  target_link_libraries(
    ${CUR_TARGET_NAME}
    PUBLIC ibmw::api::cpp
           ibmw::extensions::dds_ext
           ibmw::extensions::iceoryx_ext
           iceoryx_posh::iceoryx_posh)
endif()

# DDS + self-developed shm variant: same sources, the PUBLIC define switches
# the second driver for every TU that includes the module header.
if(IBMW_BUILD_SHM_EXTENSION)
  add_library(${CUR_TARGET_NAME}_shm STATIC)
  add_library(${CUR_TARGET_ALIAS_NAME}_shm ALIAS ${CUR_TARGET_NAME}_shm)

  target_sources(${CUR_TARGET_NAME}_shm PRIVATE ${src})

  target_include_directories(
    ${CUR_TARGET_NAME}_shm
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)

  target_compile_definitions(
    ${CUR_TARGET_NAME}_shm
    PUBLIC IBMW_MULTI_DRIVER_DEMO_WITH_SHM)

  target_link_libraries(
    ${CUR_TARGET_NAME}_shm
    PUBLIC ibmw::api::cpp
           ibmw::extensions::dds_ext
           ibmw::extensions::shm_ext)
endif()
