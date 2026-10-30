# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# shm_loaned_ctx: iox_loaned_ctx (zero-copy ctx-meta loan: CDR serialized
# straight into the shm chunk) on the self-developed shm transport.
#
# The publisher uses ShmCtxMetaTypedPublisher<T> from shm_ext (header only,
# TransportDriver virtuals, so the executable does not link shm_ext). The
# subscriber is iox_send's standard OnMessage<T> module, built from its
# sources with bench_options.topic pointing at this lane's topic -- the same
# receive side as shm_send, so the two lanes differ only in the publish path.
# The yaml loads libibmw_shm_transport.so with driver_name "iceoryx"; no RouDi.

if(NOT IBMW_BUILD_SHM_EXTENSION)
  message(STATUS "Skip shm_loaned_ctx because IBMW_BUILD_SHM_EXTENSION is OFF")
  return()
endif()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

set(IOX_SEND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iox_send)
set(SHM_LOANED_CTX_LIBS
  ibmw::api::cpp
  ibmw::core::engine
  ibmw::samples::cpp::transport_benchmark::bench_payload
  ibmw::samples::cpp::transport_benchmark::bench_common)

add_executable(shm_loaned_ctx_publisher_app
  standalone/publisher_app/main.cc
  managed/publisher_module/publisher_module.cc)
target_include_directories(shm_loaned_ctx_publisher_app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/managed
  ${CMAKE_CURRENT_SOURCE_DIR}/../iox_loaned_ctx/managed/publisher_module
  ${CMAKE_SOURCE_DIR}/src/extensions/transport)
target_link_libraries(shm_loaned_ctx_publisher_app PRIVATE
  ${SHM_LOANED_CTX_LIBS}
  ibmw::extensions::dds_types)

add_executable(shm_loaned_ctx_subscriber_app
  ${IOX_SEND_DIR}/standalone/subscriber_app/main.cc
  ${IOX_SEND_DIR}/managed/subscriber_module/subscriber_module.cc)
target_include_directories(shm_loaned_ctx_subscriber_app PRIVATE ${IOX_SEND_DIR}/managed ${IOX_SEND_DIR})
target_link_libraries(shm_loaned_ctx_subscriber_app PRIVATE ${SHM_LOANED_CTX_LIBS})

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/config/linux/bin ${CMAKE_BINARY_DIR}
  DEPENDS shm_loaned_ctx_publisher_app
          shm_loaned_ctx_subscriber_app
          ibmw_extensions_shm_ext)
