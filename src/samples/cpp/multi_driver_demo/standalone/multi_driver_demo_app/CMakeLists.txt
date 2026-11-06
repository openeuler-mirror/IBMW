# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

file(GLOB_RECURSE src ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)

# DDS + Iceoryx variant: multi_driver_demo_app
if(IBMW_BUILD_ICEORYX_EXTENSION)
  add_executable(${CUR_TARGET_NAME})
  add_executable(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

  target_sources(${CUR_TARGET_NAME} PRIVATE ${src})

  target_include_directories(
    ${CUR_TARGET_NAME}
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

  target_link_libraries(
    ${CUR_TARGET_NAME}
    PRIVATE gflags::gflags
            ibmw::core::engine
            ${CUR_SUPERIOR_NAMESPACE}::multi_driver_module)

  set_target_properties(${CUR_TARGET_NAME} PROPERTIES OUTPUT_NAME ${CUR_DIR})
endif()

# DDS + shm variant: multi_driver_shm_demo_app
if(IBMW_BUILD_SHM_EXTENSION)
  add_executable(${CUR_TARGET_NAME}_shm)
  add_executable(${CUR_TARGET_ALIAS_NAME}_shm ALIAS ${CUR_TARGET_NAME}_shm)

  target_sources(${CUR_TARGET_NAME}_shm PRIVATE ${src})

  target_include_directories(
    ${CUR_TARGET_NAME}_shm
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)

  target_link_libraries(
    ${CUR_TARGET_NAME}_shm
    PRIVATE gflags::gflags
            ibmw::core::engine
            ${CUR_SUPERIOR_NAMESPACE}::multi_driver_module_shm)

  set_target_properties(${CUR_TARGET_NAME}_shm PROPERTIES OUTPUT_NAME multi_driver_shm_demo_app)
endif()
