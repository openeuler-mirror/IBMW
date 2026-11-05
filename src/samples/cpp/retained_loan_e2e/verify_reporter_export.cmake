if(NOT DEFINED IBMW_NM OR NOT DEFINED IBMW_EXECUTABLE)
  message(FATAL_ERROR "IBMW_NM and IBMW_EXECUTABLE are required")
endif()

execute_process(
  COMMAND "${IBMW_NM}" -D --defined-only "${IBMW_EXECUTABLE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Unable to inspect Reporter host exports: ${error}")
endif()
if(NOT symbols MATCHES "ibmw_reporter_query_v1")
  message(FATAL_ERROR "${IBMW_EXECUTABLE} does not export ibmw_reporter_query_v1")
endif()
