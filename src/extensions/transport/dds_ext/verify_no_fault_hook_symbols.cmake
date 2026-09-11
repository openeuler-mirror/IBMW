execute_process(
  COMMAND "${NM}" -D "${LIB}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE symbols
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "nm -D failed: ${error}")
endif()
if(symbols MATCHES "Inject(Stage|Exact)DroppedForTest|RuntimeFaultHooksForTest|runtime_fault_hooks_for_test")
  message(FATAL_ERROR "production DDS extension exposes a runtime fault hook symbol")
endif()
