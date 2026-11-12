# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
if(NOT IBMW_BUILD_DDS_EXTENSION OR NOT IBMW_BUILD_PROTOCOLS)
  message(FATAL_ERROR "Native DDS QoS acceptance requires DDS and protocol types")
endif()
add_executable(ibmw_diag_dds_qos_peer ../fixtures/dds_qos_peer.cc)
target_link_libraries(ibmw_diag_dds_qos_peer PRIVATE ibmw::api::dds_type_support
  ibmw_schema_common_idl_gencode ${IBMW_FASTDDS_PROVIDER_TARGET})
add_werror(ibmw_diag_dds_qos_peer)
list(APPEND functional_dependencies ibmw_diag_dds_qos_peer ibmw_dds_extension)
add_dependencies(ibmw_diag_functional ibmw_diag_dds_qos_peer ibmw_dds_extension)
add_test(NAME ibmw_diag_native_dds_qos_network_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/native_qos.py"
    --cli "$<TARGET_FILE:ibmw_core_main>" --server "$<TARGET_FILE:ibmw_diag_runtime_fixture>"
    --types "$<TARGET_FILE:ibmw_diag_functional_types>" --net "$<TARGET_FILE:ibmw_extensions_net_ext>"
    --parameter "$<TARGET_FILE:ibmw_extensions_parameter_ext>" --log "$<TARGET_FILE:ibmw_extensions_log_control_ext>"
    --qos-peer "$<TARGET_FILE:ibmw_diag_dds_qos_peer>"
    --artifacts "${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>/native-dds-qos")
set_tests_properties(ibmw_diag_native_dds_qos_network_test PROPERTIES
  LABELS "ibmw_diag_functional;ibmw_diag_full;integration" TIMEOUT 180 RUN_SERIAL TRUE)
