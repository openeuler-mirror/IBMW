# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

add_subdirectory(managed/multi_driver_module)
add_subdirectory(package/multi_driver_pkg)
add_subdirectory(standalone/multi_driver_demo_app)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(CUR_INSTALL_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/config/linux)
else()
  message(FATAL_ERROR "Unsupport os")
endif()

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# One variant per enabled second driver: Iceoryx (multi_driver_demo_app,
# libmulti_driver_pkg.so) and/or self-developed shm (multi_driver_shm_demo_app,
# libmulti_driver_shm_pkg.so).
set(MULTI_DRIVER_DEMO_DEPENDS ibmw::core::main)
if(IBMW_BUILD_ICEORYX_EXTENSION)
  list(APPEND MULTI_DRIVER_DEMO_DEPENDS
       ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::multi_driver_demo_app
       ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::multi_driver_pkg)
endif()
if(IBMW_BUILD_SHM_EXTENSION)
  list(APPEND MULTI_DRIVER_DEMO_DEPENDS
       ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::multi_driver_demo_app_shm
       ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR}::multi_driver_pkg_shm
       ibmw_dds_extension
       ibmw_extensions_shm_ext)
endif()

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CUR_INSTALL_SOURCE_DIR}/bin ${CMAKE_BINARY_DIR}
  DEPENDS ${MULTI_DRIVER_DEMO_DEPENDS})
