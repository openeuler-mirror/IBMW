# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# shm_loan_v2: iox_loan_v2 (user-level zero-copy POD loan) on the
# self-developed shm transport.
#
# The publisher registers its POD topics through the public
# SenderRef::RegisterLoanedPodTopic() instead of casting to the iceoryx driver,
# then runs the same LoanedSample<T>::Borrow/Publish loop. The subscriber is
# iox_loan_v2's (LoanedSubscriber<T>::AttachBytes), built from its sources.
# The yaml loads libibmw_shm_transport.so with driver_name "iceoryx"; no RouDi.

if(NOT IBMW_BUILD_SHM_EXTENSION)
  message(STATUS "Skip shm_loan_v2 because IBMW_BUILD_SHM_EXTENSION is OFF")
  return()
endif()

string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set_namespace()

set(IOX_LOAN_V2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../iox_loan_v2)
set(SHM_LOAN_V2_LIBS
  ibmw::api::cpp
  ibmw::core::engine
  ibmw::samples::cpp::transport_benchmark::bench_payload
  ibmw::samples::cpp::transport_benchmark::bench_common
  ibmw::extensions::dds_types)

add_executable(shm_loan_v2_publisher_app
  standalone/publisher_app/main.cc
  managed/publisher_module/publisher_module.cc)
target_include_directories(shm_loan_v2_publisher_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/managed)
target_link_libraries(shm_loan_v2_publisher_app PRIVATE ${SHM_LOAN_V2_LIBS})

add_executable(shm_loan_v2_subscriber_app
  ${IOX_LOAN_V2_DIR}/standalone/subscriber_app/main.cc
  ${IOX_LOAN_V2_DIR}/managed/subscriber_module/subscriber_module.cc)
target_include_directories(shm_loan_v2_subscriber_app PRIVATE ${IOX_LOAN_V2_DIR}/managed)
target_link_libraries(shm_loan_v2_subscriber_app PRIVATE ${SHM_LOAN_V2_LIBS})

get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

add_custom_target(
  ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR}_build_all ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/config/linux/bin ${CMAKE_BINARY_DIR}
  DEPENDS shm_loan_v2_publisher_app
          shm_loan_v2_subscriber_app
          ibmw_extensions_shm_ext)
