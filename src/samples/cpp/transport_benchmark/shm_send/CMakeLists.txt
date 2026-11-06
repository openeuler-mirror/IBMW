# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# shm_send: iox_send on the self-developed shm transport (shm_ext).
#
# Same publisher/subscriber modules as iox_send (built from its sources),
# different extension in the yaml: libibmw_shm_transport.so with
# driver_name "iceoryx", so the messaging section is unchanged. This is the
# drop-in migration path for existing iceoryx deployments, and gives an
# apples-to-apples latency comparison with iox_send (no RouDi needed).

if(NOT IBMW_BUILD_SHM_EXTENSION)
  message(STATUS "Skip shm_send because IBMW_BUILD_SHM_EXTENSION is OFF")
  return()
endif()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

set(IOX_SEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iox_send)
set(SHM_SEND_LIBS
  ibmw::api::cpp
  ibmw::core::engine
  ibmw::samples::cpp::transport_benchmark::bench_payload
  ibmw::samples::cpp::transport_benchmark::bench_common)

add_executable(shm_send_publisher_app
  ${IOX_SEND_DIR}/standalone/publisher_app/main.cc
  ${IOX_SEND_DIR}/managed/publisher_module/publisher_module.cc)
target_include_directories(shm_send_publisher_app PRIVATE ${IOX_SEND_DIR}/managed ${IOX_SEND_DIR})
target_link_libraries(shm_send_publisher_app PRIVATE ${SHM_SEND_LIBS})

add_executable(shm_send_subscriber_app
  ${IOX_SEND_DIR}/standalone/subscriber_app/main.cc
  ${IOX_SEND_DIR}/managed/subscriber_module/subscriber_module.cc)
target_include_directories(shm_send_subscriber_app PRIVATE ${IOX_SEND_DIR}/managed ${IOX_SEND_DIR})
target_link_libraries(shm_send_subscriber_app PRIVATE ${SHM_SEND_LIBS})

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/config/linux/bin ${CMAKE_BINARY_DIR}
  DEPENDS shm_send_publisher_app
          shm_send_subscriber_app
          ibmw_extensions_shm_ext)
