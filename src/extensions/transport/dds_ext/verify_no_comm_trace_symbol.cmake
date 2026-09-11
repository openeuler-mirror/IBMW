execute_process(
  COMMAND "${NM}" -D "${LIB}"
  RESULT_VARIABLE nm_result
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE nm_error)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "nm failed: ${nm_error}")
endif()
if(symbols MATCHES "ibmw_dds_comm_trace")
  message(FATAL_ERROR "DDS communication trace/exporter symbol exists in an OFF build")
endif()
